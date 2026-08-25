#ifndef OEVM_HTTP_H
#define OEVM_HTTP_H
/* VM-side glue for the http driver: drain the backend ring at the pumpEvents
   safe point and post [id,method,path] Arrays to dev/http/request. */
void http_pump(void);
#endif
