-module(rfid).

-export([
    start/1, stop/1, enable_write_mode/2, disable_write_mode/1, latest_reading/1
]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-behavior(gen_server).

-define(TRACE(A, B), io:format(A, B)).
%-define(TRACE(A, B), ok).
-define(DEFAULT_CONFIG, #{
    miso_gpio => 19,
    mosi_gpio => 23,
    sck_gpio => 18,
    sda_gpio => 5
}).

-record(state, {
    port,
    config,
    latest_reading
}).


-type rfid() :: pid().
-type miso_gpio() ::non_neg_integer().
-type mosi_gpio() ::non_neg_integer().
-type sck_gpio() ::non_neg_integer().
-type sda_gpio() ::non_neg_integer().
-type config() :: #{
    miso_gpio => miso_gpio(),
    mosi_gpio => mosi_gpio(),
    sck_gpio => sck_gpio(),
    sda_gpio => sda_gpio(),
    rfid_reading_filter => undefined,
    rfid_reading_handler => fun((rfid_reading()) -> any())
}.

-type serial_number() :: [non_neg_integer()].
-type read_data() :: [non_neg_integer()].
-type write_data() :: byte().
-type write_mode() :: boolean().

-type rfid_reading() :: #{
    serial_number => serial_number(),
    read_data => read_data(),
    write_data => write_data(),
    write_mode => write_mode()
}.

%%-----------------------------------------------------------------------------
%% @param   Config      configuration
%% @returns ok | {error, Reason}
%% @doc     Start a RFID instance.
%%-----------------------------------------------------------------------------
-spec start(Config::config()) -> {ok, rfid()} | {error, Reason::term()}.
start(Config) ->
    io:format("config ~p~n", [maps:merge(?DEFAULT_CONFIG, Config)]),
    gen_server:start(?MODULE, validate_config(maps:merge(?DEFAULT_CONFIG, Config)), []).

%%-----------------------------------------------------------------------------
%% @param   RFID    the RFID instance created via `start/1'
%% @returns ok
%% @doc     Stop the specified RFID.
%% @end
%%-----------------------------------------------------------------------------
-spec stop(RFID::rfid()) -> ok.
stop(RFID) ->
    gen_server:stop(RFID).

%%-----------------------------------------------------------------------------
%% @param   RFID    the RFID instance created via `start/1'
%% @returns the latest RFID reading, or `undefined', if no reading has been taken.
%% @doc     Write data
%% @end
%%-----------------------------------------------------------------------------
-spec enable_write_mode(RFID::rfid(), Data::byte()) -> rfid_reading() | undefined | {error, Reason::term()}.
enable_write_mode(RFID, Data) ->
    io:format("enable write ~p~n", [Data]),
    gen_server:call(RFID, {enable_write_mode, Data}).

-spec disable_write_mode(RFID::rfid()) -> rfid_reading() | undefined | {error, Reason::term()}.
disable_write_mode(RFID) ->
    gen_server:call(RFID, disable_write_mode).

%%-----------------------------------------------------------------------------
%% @param   RFID    the RFID instance created via `start/1'
%% @returns the latest RFID reading, or `undefined', if no reading has been taken.
%% @doc     Return the latest RFID reading.
%% @end
%%-----------------------------------------------------------------------------
-spec latest_reading(RFID::rfid()) -> rfid_reading() | undefined | {error, Reason::term()}.
latest_reading(RFID) ->
    gen_server:call(RFID, latest_reading).

%% ====================================================================
%%
%% gen_server API
%%
%% ============================================================================

%% @hidden
init(Config) ->
    try
        Self = self(),
        Port = erlang:open_port({spawn, "atomvm_rfid"}, [{receiver, Self}, {config, Config}]),
        {ok, #state{
            port=Port,
            config=Config
        }}
    catch
        _:Error ->
            {stop, Error}
    end.

%% @hidden
handle_call(stop, _From, State) ->
    do_stop(State#state.port),
    {stop, normal, ok, State};

handle_call({enable_write_mode, Data}, _From, State) ->
    do_enable_write_mode(State#state.port, Data),
    {reply, State#state.latest_reading, State};
handle_call(disable_write_mode, _From, State) ->
    do_disable_write_mode(State#state.port),
    {reply, State#state.latest_reading, State};
handle_call(latest_reading, _From, State) ->
    {reply, State#state.latest_reading, State};
handle_call(Request, _From, State) ->
    {reply, {error, {unknown_request, Request}}, State}.

%% @hidden
handle_cast(_Msg, State) ->
    {noreply, State}.

%% @hidden
handle_info({rc522_reading, RFIDReading}, State) ->
    ?TRACE("handle_info: Received {rfid_reading, ~p}~n", [RFIDReading]),
    Config = State#state.config,
    Self = self(),
    case maps:get(rfid_reading_handler, Config, undefined) of
        undefined ->
            ok;
        Fun when is_function(Fun) ->
            NewReading = maybe_filter_rfid_reading(
                RFIDReading,
                maps:get(rfid_reading_filter, State#state.config, undefined)
            ),
            spawn(fun() -> Fun(Self, NewReading) end);
        Pid when is_pid(Pid) ->
            NewReading = maybe_filter_rfid_reading(
                RFIDReading,
                maps:get(rfid_reading_filter, State#state.config, undefined)
            ),
            Pid ! {rfid_reading, NewReading}
    end,
    erlang:garbage_collect(),
    {noreply, State#state{latest_reading=RFIDReading}};
handle_info(Info, State) ->
    io:format("Unexpected INFO message: ~p~n", [Info]),
    {noreply, State}.

%% @hidden
terminate(Reason, State) ->
    do_stop(State#state.port),
    io:format("rfid gen_server process ~p terminated with reason ~p.  State: ~p~n", [self(), Reason, State]),
    ok.

%% @hidden
code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

%%
%% internal operations
%%

%% @private
maybe_filter_rfid_reading(RFIDReading, undefined) ->
    RFIDReading;
maybe_filter_rfid_reading(RFIDReading, Keys) ->
    maps:fold(
        fun(Key, Value, Accum) ->
            case lists:member(Key, Keys) of
                true ->
                    Accum#{Key => Value};
                _ ->
                    Accum
            end
        end,
        maps:new(),
        RFIDReading
    ).

%% @private
do_enable_write_mode(Port, Data) ->
    call(Port, {enable_write, Data}).

%% @private
do_disable_write_mode(Port) ->
    call(Port, disable_write).

%% @private
do_stop(Port) ->
    call(Port, tini),
    Port ! stop.

%% @private
call(Port, Msg) ->
    Ref = make_ref(),
    Port ! {self(), Ref, Msg},
    receive
        {Ref, Ret} ->
            Ret
    end.


%% @private
validate_config(Config) ->
    validate_integer(maps:get(miso_gpio, Config)),
    validate_integer(maps:get(mosi_gpio, Config)),
    validate_integer(maps:get(sck_gpio, Config)),
    validate_integer(maps:get(sda_gpio, Config)),
    validate_atom_list_or_undefined(maps:get(rfid_reading_filter, Config, undefined)),
    validate_fun(maps:get(rfid_reading_handler, Config)),
    Config.

%% @private
validate_integer(I) when is_integer(I) ->   ok;
validate_integer(_) ->        throw(bardarg).

%% @private
validate_atom_list_or_undefined(undefined) -> ok;
validate_atom_list_or_undefined(List) when is_list(List) ->
    lists:foreach(fun validate_atom/1, List);
validate_atom_list_or_undefined(_) ->       throw(bardarg).

%% @private
validate_atom(Atom) when is_atom(Atom) -> ok;
validate_atom(_) ->       throw(bardarg).

%% @private
validate_fun(Fun) when is_function(Fun) -> ok;
validate_fun(_) ->       throw(bardarg).
