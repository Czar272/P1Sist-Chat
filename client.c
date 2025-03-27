//gcc client.c -o client -lwebsockets

#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_MESSAGE_LEN 512
#define MAX_NAME_LEN 50

static struct lws *global_wsi;
static char username[MAX_NAME_LEN];
static int registrado = 0;  // Variable para saber si el usuario ya está registrado

static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("Conexión WebSocket establecida\n");
            global_wsi = wsi;
            // Enviar mensaje de registro si no está registrado
            if (!registrado) {
                char mensaje[MAX_MESSAGE_LEN];
                snprintf(mensaje, sizeof(mensaje),
                         "{\"type\": \"register\", \"sender\": \"%s\"}", username);
                registrado = 1;

                size_t mensaje_len = strlen(mensaje);
                unsigned char buffer[LWS_PRE + MAX_MESSAGE_LEN];
                memcpy(&buffer[LWS_PRE], mensaje, mensaje_len);
                lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
            }
            // Después de enviar el mensaje de registro, activar el flujo de escritura para mensajes
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            char mensaje_usuario[MAX_MESSAGE_LEN];
            printf("Escribir mensaje: ");
            if (fgets(mensaje_usuario, sizeof(mensaje_usuario), stdin)) {
                mensaje_usuario[strcspn(mensaje_usuario, "\n")] = 0; // Elimina el salto de línea

                // Crear JSON {"mensaje": "texto"}
                char json_mensaje[MAX_MESSAGE_LEN];
                snprintf(json_mensaje, sizeof(json_mensaje), 
                    "{\"type\": \"message\", \"sender\": \"%s\", \"mensaje\": \"%s\"}",
                    username, mensaje_usuario);

                size_t mensaje_len = strlen(json_mensaje);
                unsigned char buffer[LWS_PRE + MAX_MESSAGE_LEN];
                memcpy(&buffer[LWS_PRE], json_mensaje, mensaje_len);

                // Enviar JSON
                lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
            }
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLOSED:
            printf("Conexión cerrada\n");
            global_wsi = NULL;
            break;

        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "chat-protocol", callback_client, 0, 0 },
    { NULL, NULL, 0, 0 }
};

int main() {
    printf("Ingrese su nombre de usuario: ");
    fgets(username, sizeof(username), stdin);
    username[strcspn(username, "\n")] = 0; // Elimina el salto de línea

    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Error al crear contexto\n");
        return 1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = context;
    ccinfo.address = "localhost";
    ccinfo.port = 8000;
    ccinfo.path = "/";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "chat-protocol";

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "Error al conectar con el servidor WebSocket\n");
        return 1;
    }

    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);
    return 0;
}