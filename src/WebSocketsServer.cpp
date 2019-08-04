/**
 * @file WebSocketsServer.cpp
 * @date 20.05.2015
 * @author Markus Sattler
 *
 * Copyright (c) 2015 Markus Sattler. All rights reserved.
 * This file is part of the WebSockets for Arduino.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "WebSockets.h"
#include "WebSocketsServer.h"

WebSocketsServer::WebSocketsServer(uint16_t port, String origin, String protocol) {
    _port     = port;
    _origin   = origin;
    _protocol = protocol;
    _runnning = false;

    _server = new WEBSOCKETS_NETWORK_SERVER_CLASS(port);

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
    _server->onClient([](void * s, AsyncClient * c) {
        ((WebSocketsServer *)s)->newClient(new AsyncTCPbuffer(c));
    },
        this);
#endif

    _cbEvent = NULL;

    _httpHeaderValidationFunc = NULL;
    _mandatoryHttpHeaders     = NULL;
    _mandatoryHttpHeaderCount = 0;

    memset(&_clients[0], 0x00, (sizeof(WSclient_t) * WEBSOCKETS_SERVER_CLIENT_MAX));
}

WebSocketsServer::~WebSocketsServer() {
    // disconnect all clients
    close();

    if(_mandatoryHttpHeaders)
        delete[] _mandatoryHttpHeaders;

    _mandatoryHttpHeaderCount = 0;
}

/**
 * called to initialize the Websocket server
 */
void WebSocketsServer::begin(void) {
    WSclient_t * client;

    // init client storage
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];

        client->num    = i;
        client->status = WSC_NOT_CONNECTED;
        client->tcp    = NULL;
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
        client->isSSL = false;
        client->ssl   = NULL;
#endif
        client->cUrl         = "";
        client->cCode        = 0;
        client->cKey         = "";
        client->cProtocol    = "";
        client->cVersion     = 0;
        client->cIsUpgrade   = false;
        client->cIsWebsocket = false;

        client->base64Authorization = "";

        client->cWsRXsize = 0;

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
        client->cHttpLine = "";
#endif
    }

#ifdef ESP8266
    randomSeed(RANDOM_REG32);
#elif defined(ESP32)
#define DR_REG_RNG_BASE 0x3ff75144
    randomSeed(READ_PERI_REG(DR_REG_RNG_BASE));
#else
    // TODO find better seed
    randomSeed(millis());
#endif

    _runnning = true;
    _server->begin();

    ESP_LOGD(TAG, "Server Started.");
}

void WebSocketsServer::close(void) {
    _runnning = false;
    disconnect();

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)
    _server->close();
#elif(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
    _server->end();
#else
    // TODO how to close server?
#endif
}

#if(WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
/**
 * called in arduino loop
 */
void WebSocketsServer::loop(void) {
    if(_runnning) {
        handleNewClients();
        handleClientData();
    }
}
#endif

/**
 * set callback function
 * @param cbEvent WebSocketServerEvent
 */
void WebSocketsServer::onEvent(WebSocketServerEvent cbEvent) {
    _cbEvent = cbEvent;
}

/*
 * Sets the custom http header validator function
 * @param httpHeaderValidationFunc WebSocketServerHttpHeaderValFunc ///< pointer to the custom http header validation function
 * @param mandatoryHttpHeaders[] const char* ///< the array of named http headers considered to be mandatory / must be present in order for websocket upgrade to succeed
 * @param mandatoryHttpHeaderCount size_t ///< the number of items in the mandatoryHttpHeaders array
 */
void WebSocketsServer::onValidateHttpHeader(
    WebSocketServerHttpHeaderValFunc validationFunc,
    const char * mandatoryHttpHeaders[],
    size_t mandatoryHttpHeaderCount) {
    _httpHeaderValidationFunc = validationFunc;

    if(_mandatoryHttpHeaders)
        delete[] _mandatoryHttpHeaders;

    _mandatoryHttpHeaderCount = mandatoryHttpHeaderCount;
    _mandatoryHttpHeaders     = new String[_mandatoryHttpHeaderCount];

    for(size_t i = 0; i < _mandatoryHttpHeaderCount; i++) {
        _mandatoryHttpHeaders[i] = mandatoryHttpHeaders[i];
    }
}

/*
 * send text data to client
 * @param num uint8_t client id
 * @param payload uint8_t *
 * @param length size_t
 * @param headerToPayload bool  (see sendFrame for more details)
 * @return true if ok
 */
bool WebSocketsServer::sendTXT(uint8_t num, uint8_t * payload, size_t length, bool headerToPayload) {
    if(num >= WEBSOCKETS_SERVER_CLIENT_MAX) {
        return false;
    }
    if(length == 0) {
        length = strlen((const char *)payload);
    }
    WSclient_t * client = &_clients[num];
    if(clientIsConnected(client)) {
        return sendFrame(client, WSop_text, payload, length, true, headerToPayload);
    }
    return false;
}

bool WebSocketsServer::sendTXT(uint8_t num, const uint8_t * payload, size_t length) {
    return sendTXT(num, (uint8_t *)payload, length);
}

bool WebSocketsServer::sendTXT(uint8_t num, char * payload, size_t length, bool headerToPayload) {
    return sendTXT(num, (uint8_t *)payload, length, headerToPayload);
}

bool WebSocketsServer::sendTXT(uint8_t num, const char * payload, size_t length) {
    return sendTXT(num, (uint8_t *)payload, length);
}

bool WebSocketsServer::sendTXT(uint8_t num, String & payload) {
    return sendTXT(num, (uint8_t *)payload.c_str(), payload.length());
}

/**
 * send text data to client all
 * @param payload uint8_t *
 * @param length size_t
 * @param headerToPayload bool  (see sendFrame for more details)
 * @return true if ok
 */
bool WebSocketsServer::broadcastTXT(uint8_t * payload, size_t length, bool headerToPayload) {
    WSclient_t * client;
    bool ret = true;
    if(length == 0) {
        length = strlen((const char *)payload);
    }

    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(clientIsConnected(client)) {
            if(!sendFrame(client, WSop_text, payload, length, true, headerToPayload)) {
                ret = false;
            }
        }
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)
        delay(0);
#endif
    }
    return ret;
}

bool WebSocketsServer::broadcastTXT(const uint8_t * payload, size_t length) {
    return broadcastTXT((uint8_t *)payload, length);
}

bool WebSocketsServer::broadcastTXT(char * payload, size_t length, bool headerToPayload) {
    return broadcastTXT((uint8_t *)payload, length, headerToPayload);
}

bool WebSocketsServer::broadcastTXT(const char * payload, size_t length) {
    return broadcastTXT((uint8_t *)payload, length);
}

bool WebSocketsServer::broadcastTXT(String & payload) {
    return broadcastTXT((uint8_t *)payload.c_str(), payload.length());
}

/**
 * send binary data to client
 * @param num uint8_t client id
 * @param payload uint8_t *
 * @param length size_t
 * @param headerToPayload bool  (see sendFrame for more details)
 * @return true if ok
 */
bool WebSocketsServer::sendBIN(uint8_t num, uint8_t * payload, size_t length, bool headerToPayload) {
    if(num >= WEBSOCKETS_SERVER_CLIENT_MAX) {
        return false;
    }
    WSclient_t * client = &_clients[num];
    if(clientIsConnected(client)) {
        return sendFrame(client, WSop_binary, payload, length, true, headerToPayload);
    }
    return false;
}

bool WebSocketsServer::sendBIN(uint8_t num, const uint8_t * payload, size_t length) {
    return sendBIN(num, (uint8_t *)payload, length);
}

/**
 * send binary data to client all
 * @param payload uint8_t *
 * @param length size_t
 * @param headerToPayload bool  (see sendFrame for more details)
 * @return true if ok
 */
bool WebSocketsServer::broadcastBIN(uint8_t * payload, size_t length, bool headerToPayload) {
    WSclient_t * client;
    bool ret = true;
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(clientIsConnected(client)) {
            if(!sendFrame(client, WSop_binary, payload, length, true, headerToPayload)) {
                ret = false;
            }
        }
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)
        delay(0);
#endif
    }
    return ret;
}

bool WebSocketsServer::broadcastBIN(const uint8_t * payload, size_t length) {
    return broadcastBIN((uint8_t *)payload, length);
}

/**
 * sends a WS ping to Client
 * @param num uint8_t client id
 * @param payload uint8_t *
 * @param length size_t
 * @return true if ping is send out
 */
bool WebSocketsServer::sendPing(uint8_t num, uint8_t * payload, size_t length) {
    if(num >= WEBSOCKETS_SERVER_CLIENT_MAX) {
        return false;
    }
    WSclient_t * client = &_clients[num];
    if(clientIsConnected(client)) {
        return sendFrame(client, WSop_ping, payload, length);
    }
    return false;
}

bool WebSocketsServer::sendPing(uint8_t num, String & payload) {
    return sendPing(num, (uint8_t *)payload.c_str(), payload.length());
}

/**
 *  sends a WS ping to all Client
 * @param payload uint8_t *
 * @param length size_t
 * @return true if ping is send out
 */
bool WebSocketsServer::broadcastPing(uint8_t * payload, size_t length) {
    WSclient_t * client;
    bool ret = true;
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(clientIsConnected(client)) {
            if(!sendFrame(client, WSop_ping, payload, length)) {
                ret = false;
            }
        }
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)
        delay(0);
#endif
    }
    return ret;
}

bool WebSocketsServer::broadcastPing(String & payload) {
    return broadcastPing((uint8_t *)payload.c_str(), payload.length());
}

/**
 * disconnect all clients
 */
void WebSocketsServer::disconnect(void) {
    WSclient_t * client;
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(clientIsConnected(client)) {
            WebSockets::clientDisconnect(client, 1000);
        }
    }
}

/**
 * disconnect one client
 * @param num uint8_t client id
 */
void WebSocketsServer::disconnect(uint8_t num) {
    if(num >= WEBSOCKETS_SERVER_CLIENT_MAX) {
        return;
    }
    WSclient_t * client = &_clients[num];
    if(clientIsConnected(client)) {
        WebSockets::clientDisconnect(client, 1000);
    }
}

/*
 * set the Authorization for the http request
 * @param user const char *
 * @param password const char *
 */
void WebSocketsServer::setAuthorization(const char * user, const char * password) {
    if(user && password) {
        String auth = user;
        auth += ":";
        auth += password;
        _base64Authorization = base64_encode((uint8_t *)auth.c_str(), auth.length());
    }
}

/**
 * set the Authorizatio for the http request
 * @param auth const char * base64
 */
void WebSocketsServer::setAuthorization(const char * auth) {
    if(auth) {
        _base64Authorization = auth;
    }
}

/**
 * count the connected clients (optional ping them)
 * @param ping bool ping the connected clients
 */
int WebSocketsServer::connectedClients(bool ping) {
    WSclient_t * client;
    int count = 0;
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(client->status == WSC_CONNECTED) {
            if(ping != true || sendPing(i)) {
                count++;
            }
        }
    }
    return count;
}

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
/**
 * get an IP for a client
 * @param num uint8_t client id
 * @return IPAddress
 */
IPAddress WebSocketsServer::remoteIP(uint8_t num) {
    if(num < WEBSOCKETS_SERVER_CLIENT_MAX) {
        WSclient_t * client = &_clients[num];
        if(clientIsConnected(client)) {
            return client->tcp->remoteIP();
        }
    }

    return IPAddress();
}
#endif

//#################################################################################
//#################################################################################
//#################################################################################

/**
 * handle new client connection
 * @param client
 */
bool WebSocketsServer::newClient(WEBSOCKETS_NETWORK_CLASS * TCPclient) {
    WSclient_t * client;
    // search free list entry for client
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];

        // state is not connected or tcp connection is lost
        if(!clientIsConnected(client)) {
            client->tcp = TCPclient;

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
            client->isSSL = false;
            client->tcp->setNoDelay(true);
#endif
#if(WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
            // set Timeout for readBytesUntil and readStringUntil
            client->tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT);
#endif
            client->status = WSC_HEADER;
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
#if CORE_DEBUG_LEVEL >= 4
            IPAddress ip = client->tcp->remoteIP();
#endif
            ESP_LOGD(TAG, "[%d] new client from %d.%d.%d.%d", client->num, ip[0], ip[1], ip[2], ip[3]);
#else
            ESP_LOGD(TAG, "[%d] new client", client->num);
#endif

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
            client->tcp->onDisconnect(std::bind([](WebSocketsServer * server, AsyncTCPbuffer * obj, WSclient_t * client) -> bool {
                ESP_LOGD(TAG, "[%d] Disconnect client", client->num);

                AsyncTCPbuffer ** sl = &server->_clients[client->num].tcp;
                if(*sl == obj) {
                    client->status = WSC_NOT_CONNECTED;
                    *sl            = NULL;
                }
                return true;
            },
                this, std::placeholders::_1, client));

            client->tcp->readStringUntil('\n', &(client->cHttpLine), std::bind(&WebSocketsServer::handleHeader, this, client, &(client->cHttpLine)));
#endif

            return true;
            break;
        }
    }
    return false;
}

/**
 *
 * @param client WSclient_t *  ptr to the client struct
 * @param opcode WSopcode_t
 * @param payload  uint8_t *
 * @param length size_t
 */
void WebSocketsServer::messageReceived(WSclient_t * client, WSopcode_t opcode, uint8_t * payload, size_t length, bool fin) {
    WStype_t type = WStype_ERROR;

    switch(opcode) {
        case WSop_text:
            type = fin ? WStype_TEXT : WStype_FRAGMENT_TEXT_START;
            break;
        case WSop_binary:
            type = fin ? WStype_BIN : WStype_FRAGMENT_BIN_START;
            break;
        case WSop_continuation:
            type = fin ? WStype_FRAGMENT_FIN : WStype_FRAGMENT;
            break;
        case WSop_ping:
            type = WStype_PING;
            break;
        case WSop_pong:
            type = WStype_PONG;
            break;
        case WSop_close:
        default:
            break;
    }

    runCbEvent(client->num, type, payload, length);
}

/**
 * Disconnect an client
 * @param client WSclient_t *  ptr to the client struct
 */
void WebSocketsServer::clientDisconnect(WSclient_t * client) {
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
    if(client->isSSL && client->ssl) {
        if(client->ssl->connected()) {
            client->ssl->flush();
            client->ssl->stop();
        }
        delete client->ssl;
        client->ssl = NULL;
        client->tcp = NULL;
    }
#endif

    if(client->tcp) {
        if(client->tcp->connected()) {
#if(WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
            client->tcp->flush();
#endif
            client->tcp->stop();
        }
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
        client->status = WSC_NOT_CONNECTED;
#else
        delete client->tcp;
#endif
        client->tcp = NULL;
    }

    client->cUrl         = "";
    client->cKey         = "";
    client->cProtocol    = "";
    client->cVersion     = 0;
    client->cIsUpgrade   = false;
    client->cIsWebsocket = false;

    client->cWsRXsize = 0;

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
    client->cHttpLine = "";
#endif

    client->status = WSC_NOT_CONNECTED;

    ESP_LOGD(TAG, "[%d] client disconnected.", client->num);

    runCbEvent(client->num, WStype_DISCONNECTED, NULL, 0);
}

/**
 * get client state
 * @param client WSclient_t *  ptr to the client struct
 * @return true = connected
 */
bool WebSocketsServer::clientIsConnected(WSclient_t * client) {
    if(!client->tcp) {
        return false;
    }

    if(client->tcp->connected()) {
        if(client->status != WSC_NOT_CONNECTED) {
            return true;
        }
    } else {
        // client lost
        if(client->status != WSC_NOT_CONNECTED) {
            ESP_LOGD(TAG, "[%d] client connection lost.", client->num);
            // do cleanup
            clientDisconnect(client);
        }
    }

    if(client->tcp) {
        // do cleanup
        ESP_LOGD(TAG, "[%d] client list cleanup.", client->num);
        clientDisconnect(client);
    }

    return false;
}
#if(WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)
/**
 * Handle incoming Connection Request
 */
void WebSocketsServer::handleNewClients(void) {
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
    while(_server->hasClient()) {
#endif
        bool ok = false;

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
        // store new connection
        WEBSOCKETS_NETWORK_CLASS * tcpClient = new WEBSOCKETS_NETWORK_CLASS(_server->available());
#else
    WEBSOCKETS_NETWORK_CLASS * tcpClient = new WEBSOCKETS_NETWORK_CLASS(_server->available());
#endif

        if(!tcpClient) {
            ESP_LOGD(TAG, "creating Network class failed!");
            return;
        }

        ok = newClient(tcpClient);

        if(!ok) {
            // no free space to handle client
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
#if CORE_DEBUG_LEVEL >= 4
            IPAddress ip = tcpClient->remoteIP();
#endif
            ESP_LOGD(TAG, "no free space new client from %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
#else
        ESP_LOGD(TAG, "no free space new client");
#endif
            tcpClient->stop();
        }

#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266) || (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP32)
        delay(0);
    }
#endif
}

/**
 * Handel incomming data from Client
 */
void WebSocketsServer::handleClientData(void) {
    WSclient_t * client;
    for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        client = &_clients[i];
        if(clientIsConnected(client)) {
            int len = client->tcp->available();
            if(len > 0) {
                //ESP_LOGD(TAG, "[%d][handleClientData] len: %d", client->num, len);
                switch(client->status) {
                    case WSC_HEADER: {
                        String headerLine = client->tcp->readStringUntil('\n');
                        handleHeader(client, &headerLine);
                    } break;
                    case WSC_CONNECTED:
                        WebSockets::handleWebsocket(client);
                        break;
                    default:
                        WebSockets::clientDisconnect(client, 1002);
                        break;
                }
            }
        }
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)
        delay(0);
#endif
    }
}
#endif

/*
 * returns an indicator whether the given named header exists in the configured _mandatoryHttpHeaders collection
 * @param headerName String ///< the name of the header being checked
 */
bool WebSocketsServer::hasMandatoryHeader(String headerName) {
    for(size_t i = 0; i < _mandatoryHttpHeaderCount; i++) {
        if(_mandatoryHttpHeaders[i].equalsIgnoreCase(headerName))
            return true;
    }
    return false;
}

/**
 * handles http header reading for WebSocket upgrade
 * @param client WSclient_t * ///< pointer to the client struct
 * @param headerLine String ///< the header being read / processed
 */
void WebSocketsServer::handleHeader(WSclient_t * client, String * headerLine) {
    static const char * NEW_LINE = "\r\n";

    headerLine->trim();    // remove \r

    if(headerLine->length() > 0) {
        ESP_LOGD(TAG, "[%d][handleHeader] RX: %s", client->num, headerLine->c_str());

        // websocket requests always start with GET see rfc6455
        if(headerLine->startsWith("GET ")) {
            // cut URL out
            client->cUrl = headerLine->substring(4, headerLine->indexOf(' ', 4));

            //reset non-websocket http header validation state for this client
            client->cHttpHeadersValid      = true;
            client->cMandatoryHeadersCount = 0;

        } else if(headerLine->indexOf(':') >= 0) {
            String headerName  = headerLine->substring(0, headerLine->indexOf(':'));
            String headerValue = headerLine->substring(headerLine->indexOf(':') + 1);

            // remove space in the beginning (RFC2616)
            if(headerValue[0] == ' ') {
                headerValue.remove(0, 1);
            }

            if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Connection"))) {
                headerValue.toLowerCase();
                if(headerValue.indexOf(WEBSOCKETS_STRING("upgrade")) >= 0) {
                    client->cIsUpgrade = true;
                }
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Upgrade"))) {
                if(headerValue.equalsIgnoreCase(WEBSOCKETS_STRING("websocket"))) {
                    client->cIsWebsocket = true;
                }
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Sec-WebSocket-Version"))) {
                client->cVersion = headerValue.toInt();
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Sec-WebSocket-Key"))) {
                client->cKey = headerValue;
                client->cKey.trim();    // see rfc6455
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Sec-WebSocket-Protocol"))) {
                client->cProtocol = headerValue;
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Sec-WebSocket-Extensions"))) {
                client->cExtensions = headerValue;
            } else if(headerName.equalsIgnoreCase(WEBSOCKETS_STRING("Authorization"))) {
                client->base64Authorization = headerValue;
            } else {
                client->cHttpHeadersValid &= execHttpHeaderValidation(headerName, headerValue);
                if(_mandatoryHttpHeaderCount > 0 && hasMandatoryHeader(headerName)) {
                    client->cMandatoryHeadersCount++;
                }
            }

        } else {
            ESP_LOGD(TAG, "[handleHeader] Header error (%s)", headerLine->c_str());
        }

        (*headerLine) = "";
#if(WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266_ASYNC)
        client->tcp->readStringUntil('\n', &(client->cHttpLine), std::bind(&WebSocketsServer::handleHeader, this, client, &(client->cHttpLine)));
#endif
    } else {
        ESP_LOGD(TAG, "[%d][handleHeader] Header read fin.", client->num);
        ESP_LOGD(TAG, "[%d][handleHeader]  - cURL: %s", client->num, client->cUrl.c_str());
        ESP_LOGD(TAG, "[%d][handleHeader]  - cIsUpgrade: %d", client->num, client->cIsUpgrade);
        ESP_LOGD(TAG, "[%d][handleHeader]  - cIsWebsocket: %d", client->num, client->cIsWebsocket);
        ESP_LOGD(TAG, "[%d][handleHeader]  - cKey: %s", client->num, client->cKey.c_str());
        ESP_LOGD(TAG, "[%d][handleHeader]  - cProtocol: %s", client->num, client->cProtocol.c_str());
        ESP_LOGD(TAG, "[%d][handleHeader]  - cExtensions: %s", client->num, client->cExtensions.c_str());
        ESP_LOGD(TAG, "[%d][handleHeader]  - cVersion: %d", client->num, client->cVersion);
        ESP_LOGD(TAG, "[%d][handleHeader]  - base64Authorization: %s", client->num, client->base64Authorization.c_str());
        ESP_LOGD(TAG, "[%d][handleHeader]  - cHttpHeadersValid: %d", client->num, client->cHttpHeadersValid);
        ESP_LOGD(TAG, "[%d][handleHeader]  - cMandatoryHeadersCount: %d", client->num, client->cMandatoryHeadersCount);

        bool ok = (client->cIsUpgrade && client->cIsWebsocket);

        if(ok) {
            if(client->cUrl.length() == 0) {
                ok = false;
            }
            if(client->cKey.length() == 0) {
                ok = false;
            }
            if(client->cVersion != 13) {
                ok = false;
            }
            if(!client->cHttpHeadersValid) {
                ok = false;
            }
            if(client->cMandatoryHeadersCount != _mandatoryHttpHeaderCount) {
                ok = false;
            }
        }

        if(_base64Authorization.length() > 0) {
            String auth = WEBSOCKETS_STRING("Basic ");
            auth += _base64Authorization;
            if(auth != client->base64Authorization) {
                ESP_LOGD(TAG, "[%d][handleHeader] HTTP Authorization failed!", client->num);
                handleAuthorizationFailed(client);
                return;
            }
        }

        if(ok) {
            ESP_LOGD(TAG, "[%d][handleHeader] Websocket connection incoming.", client->num);

            // generate Sec-WebSocket-Accept key
            String sKey = acceptKey(client->cKey);

            ESP_LOGD(TAG, "[%d][handleHeader]  - sKey: %s", client->num, sKey.c_str());

            client->status = WSC_CONNECTED;

            String handshake = WEBSOCKETS_STRING(
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Server: arduino-WebSocketsServer\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Accept: ");
            handshake += sKey + NEW_LINE;

            if(_origin.length() > 0) {
                handshake += WEBSOCKETS_STRING("Access-Control-Allow-Origin: ");
                handshake += _origin + NEW_LINE;
            }

            if(client->cProtocol.length() > 0) {
                handshake += WEBSOCKETS_STRING("Sec-WebSocket-Protocol: ");
                handshake += _protocol + NEW_LINE;
            }

            // header end
            handshake += NEW_LINE;

            ESP_LOGD(TAG, "[%d][handleHeader] handshake %s", client->num, (uint8_t *)handshake.c_str());

            write(client, (uint8_t *)handshake.c_str(), handshake.length());

            headerDone(client);

            // send ping
            WebSockets::sendFrame(client, WSop_ping);

            runCbEvent(client->num, WStype_CONNECTED, (uint8_t *)client->cUrl.c_str(), client->cUrl.length());

        } else {
            handleNonWebsocketConnection(client);
        }
    }
}
