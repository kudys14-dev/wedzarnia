// web_server.h
#pragma once
#include <WebSocketsServer.h>

void web_server_init();
void web_server_handle_client();
void web_server_ws_broadcast();   // wywołuj co ~1s z taskWeb
bool requireAuth();
