#include <atom.h>
#include <bif.h>
#include <context.h>
#include <debug.h>
#include <esp32_sys.h>
#include <defaultatoms.h>
#include <globalcontext.h>
#include <interop.h>
#include <mailbox.h>
#include <module.h>
#include <port.h>
#include <scheduler.h>
#include <term.h>
#include <utils.h>

#include <esp_log.h>

#include <rc522.h>

//#define ENABLE_TRACE
#include "trace.h"

#include "atomvm_rfid.h"

#define TAG "atomvm_rfid"

#define NUM_ENTRIES 4

static const char *const stop_atom =                    "\x4" "stop";
static const char *const enable_write_atom =            "\xC" "enable_write";
static const char *const disable_write_atom =           "\xD" "disable_write";
static const char *const receiver_atom =                "\x8" "receiver";
static const char *const config_atom =                  "\x6" "config";
static const char *const miso_gpio_atom =               "\x9" "miso_gpio";
static const char *const mosi_gpio_atom =               "\x9" "mosi_gpio";
static const char *const sck_gpio_atom =                "\x8" "sck_gpio";
static const char *const sda_gpio_atom =                "\x8" "sda_gpio";

static const char *const serial_number_atom =           "\xD" "serial_number";
static const char *const read_data_atom =               "\x9" "read_data";
static const char *const write_data_atom =              "\xA" "write_data";
static const char *const write_mode_atom =              "\xA" "write_mode";
static const char *const rc522_reading_atom =           "\xD" "rc522_reading";
static const char *const rc522_request_atom =           "\xD" "rc522_request";



struct platform_data {
    rc522_handle_t parser;
    term receiver;
};

static int get_integer_value(Context *ctx, term config, term key, int default_value)
{
    term value = interop_map_get_value_default(
        ctx, config, key, term_invalid_term()
    );
    if (term_is_invalid_term(value)) {
        return default_value;
    } else if (term_is_integer(value)) {
        return term_to_int(value);
    }
    ESP_LOGE(TAG, "Invalid integer value.");
    return -1;
}

static int get_miso_gpio(Context *ctx, term config)
{
    return get_integer_value(ctx, config, context_make_atom(ctx, miso_gpio_atom), 19);
}

static int get_mosi_gpio(Context *ctx, term config)
{
    return get_integer_value(ctx, config, context_make_atom(ctx, mosi_gpio_atom), 23);
}

static int get_sck_gpio(Context *ctx, term config)
{
    return get_integer_value(ctx, config, context_make_atom(ctx, sck_gpio_atom), 18);
}

static int get_sda_gpio(Context *ctx, term config)
{
    return get_integer_value(ctx, config, context_make_atom(ctx, sda_gpio_atom), 5);
}

static term bool_to_term(bool b)
{
    return b ? TRUE_ATOM : FALSE_ATOM;
}

static term u64_to_term(Context *ctx, uint64_t u64)
{
    term ret = term_nil();

    for (int i = 0;  i < 8;  i++) {
        uint8_t u8 = (uint8_t)((u64 >> 8*i) & 0xFF);
        ret = term_list_prepend(term_from_int(u8), ret, ctx);
    }
    return ret;
}

static term rc522_to_term(Context *ctx, rc522_event_data_t *rc522)
{
    rc522_tag_t* tag = (rc522_tag_t*) rc522->ptr;
    term serial_number = u64_to_term(ctx, tag->serial_number);
    term read_data = u64_to_term(ctx, tag->read_data);
    term write_data = u64_to_term(ctx, tag->write_data);
    term write_mode = bool_to_term(tag->write_mode);
    term map = term_alloc_map(ctx, NUM_ENTRIES);
    term_set_map_assoc(map, 0, context_make_atom(ctx, serial_number_atom), serial_number);
    term_set_map_assoc(map, 1, context_make_atom(ctx, read_data_atom), read_data);
    term_set_map_assoc(map, 2, context_make_atom(ctx, write_data_atom), write_data);
    term_set_map_assoc(map, 3, context_make_atom(ctx, write_mode_atom), write_mode);

    return map;
}

static void rc522_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    Context *ctx = (Context *) event_handler_arg;
    struct platform_data *plfdat = (struct platform_data *) ctx->platform_data;

    int pid = term_to_local_process_id(plfdat->receiver);
    Context *target = globalcontext_get_process(ctx->global, pid);

    rc522_event_data_t *rc522 = NULL;
    term rc522_reading;
    term msg;
    switch (event_id) {
        case RC522_EVENT_TAG_REQUESTED:
            msg = port_create_tuple2(ctx,
                context_make_atom(ctx, rc522_request_atom),
                term_nil()
            );
            mailbox_send(target, msg);
            break;
        case RC522_EVENT_TAG_SCANNED:
            rc522 = (rc522_event_data_t *)event_data;

            if (UNLIKELY(memory_ensure_free(ctx, term_map_size_in_terms(NUM_ENTRIES) + 210) != MEMORY_GC_OK)) {
                mailbox_send(target, MEMORY_ATOM);
                return;
            }

            rc522_reading = rc522_to_term(ctx, rc522);
            msg = port_create_tuple2(ctx,
                context_make_atom(ctx, rc522_reading_atom),
                rc522_reading
            );

            mailbox_send(target, msg);

            break;
        default:
            break;
    }
}

static term do_enable_write_mode(Context *ctx, uint8_t data)
{
    struct platform_data *plfdat = (struct platform_data *) ctx->platform_data;
    rc522_handle_t rc522 = (rc522_handle_t) plfdat->parser;

    TRACE(TAG ": do_enable_write_mode\n");
    rc522_enable_write_mode(rc522, data);
    return OK_ATOM;
}

static term do_disable_write_mode(Context *ctx)
{
    struct platform_data *plfdat = (struct platform_data *) ctx->platform_data;
    rc522_handle_t rc522 = (rc522_handle_t) plfdat->parser;

    TRACE(TAG ": do_disable_write_mode\n");
    rc522_disable_write_mode(rc522);
    return OK_ATOM;
}

static void do_stop(Context *ctx)
{
    struct platform_data *plfdat = (struct platform_data *) ctx->platform_data;
    rc522_handle_t parser = plfdat->parser;

    TRACE(TAG ": do_stop\n");
    rc522_destroy(parser);
    scheduler_terminate(ctx);
    free(plfdat);
}

static void consume_mailbox(Context *ctx)
{
    Message *message = mailbox_dequeue(ctx);
    term msg = message->message;
    term pid = term_get_tuple_element(msg, 0);
    term ref = term_get_tuple_element(msg, 1);
    uint64_t ref_ticks = term_to_ref_ticks(ref);
    term req = term_get_tuple_element(msg, 2);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    term ret = ERROR_ATOM;
    if (term_is_atom(req)) {
        if (req == context_make_atom(ctx, stop_atom)) {
            do_stop(ctx);
            ret = OK_ATOM;
        } else if (req == context_make_atom(ctx, disable_write_atom)) {
            ret = do_disable_write_mode(ctx);
        }
    } else if (term_is_tuple(req)) {
        term cmd = term_get_tuple_element(req, 0);
        if (cmd == context_make_atom(ctx, enable_write_atom)) {
            term data = term_get_tuple_element(req, 1);
            uint8_t data_u8 = term_to_uint8(data);
            ret = do_enable_write_mode(ctx, data_u8);
        }
    }

    mailbox_destroy_message(message);

    if (UNLIKELY(memory_ensure_free(ctx, 3 + 2) != MEMORY_GC_OK)) {
        mailbox_send(target, MEMORY_ATOM);
    } else {
        term ret_msg = port_create_tuple2(ctx, term_from_ref_ticks(ref_ticks, ctx), ret);
        mailbox_send(target, ret_msg);
    }
}

static term make_atom(GlobalContext *global, const char *string)
{
    int global_atom_index = globalcontext_insert_atom(global, (AtomString) string);
    return term_from_atom_index(global_atom_index);
}

//
// Entrypoints
//

void atomvm_rfid_init(GlobalContext *global)
{
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGI(TAG, "AtomVM RFID driver initialized.");
}

Context *atomvm_rfid_create_port(GlobalContext *global, term opts)
{
    term receiver = interop_proplist_get_value(opts, make_atom(global, receiver_atom));
    term config = interop_proplist_get_value(opts, make_atom(global, config_atom));


    Context *ctx = context_new(global);
    ctx->native_handler = consume_mailbox;

    rc522_config_t parser_config =
    {
        .spi = {
              .host = VSPI_HOST,
              .miso_gpio = get_miso_gpio(ctx, config),
              .mosi_gpio = get_mosi_gpio(ctx, config),
              .sck_gpio = get_sck_gpio(ctx, config),
              .sda_gpio = get_sda_gpio(ctx, config)
        }
    };

    rc522_handle_t parser;
    rc522_create(&parser_config, &parser);
    if (UNLIKELY(IS_NULL_PTR(parser))) {
        context_destroy(ctx);
        ESP_LOGE(TAG, "Error: Unable to initialize rc522 parser.\n");
        return NULL;
    }

    esp_err_t err = rc522_register_events(parser, RC522_EVENT_ANY, rc522_event_handler, ctx);
    if (err != ESP_OK) {
        context_destroy(ctx);
        rc522_destroy(parser);
        ESP_LOGE(TAG, "Error: Unable to add rc522 handler.  Error: %i.\n", err);
        return NULL;
    }
    rc522_start(parser);
    struct platform_data *plfdat = malloc(sizeof(struct platform_data));
    plfdat->parser = parser;
    plfdat->receiver = receiver;
    ctx->platform_data = plfdat;

    ESP_LOGI(TAG, "atomvm_rfid started.");
    return ctx;
}

#include <sdkconfig.h>
#ifdef CONFIG_AVM_RFID_ENABLE
REGISTER_PORT_DRIVER(atomvm_rfid, atomvm_rfid_init, atomvm_rfid_create_port)
#endif
