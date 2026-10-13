// libdatalink.so: tiny library whose constructor runs datalink::run() as early as possible.
// Loaded from Java with System.loadLibrary("datalink") (see README).
#include "datalink.h"
__attribute__((constructor)) static void datalinkInit() {
  datalink::Ctx c;
  if (datalink::detect(c)) datalink::run(c);
}
