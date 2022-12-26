#ifndef __ATOMVM_RFID_H__
#define __ATOMVM_RFID_H__

#include "context.h"
#include "globalcontext.h"
#include "term.h"

void atomvm_rfid_init(GlobalContext *global);
Context *atomvm_rfid_create_port(GlobalContext *global, term opts);

#endif
