//gcc client.c -o client -lwebsockets -ljansson
// npx wscat -c ws://localhost:8000

#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jansson.h>

#define MAX_MESSAGE_LEN 512
#define MAX_NAME_LEN 50

static struct lws *global_wsi;
static char username[MAX_NAME_LEN];
static int awaiting_response = 0;

static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("Conexión WebSocket establecida\n");
            global_wsi = wsi;

            char mensaje[MAX_MESSAGE_LEN];
            snprintf(mensaje, sizeof(mensaje),
                        "{\"type\": \"register\", \"sender\": \"%s\"}", username);

            size_t mensaje_len = strlen(mensaje);
            unsigned char buffer[LWS_PRE + MAX_MESSAGE_LEN];
            memcpy(&buffer[LWS_PRE], mensaje, mensaje_len);
            lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);

            // Activar el flujo de escritura
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (awaiting_response) {
                break;
            }

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

        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (!awaiting_response) {
                break;
            }

            char mensaje_local[MAX_MESSAGE_LEN];
            memcpy(mensaje_local, in, len);
            mensaje_local[len] = '\0'; // Asegurar que sea una cadena válida
            
            // Parsear JSON
            json_t *root;
            json_error_t error;
            root = json_loads(mensaje_local, 0, &error);
    
            if (!root) {
                printf("Error al parsear JSON: %s\n", error.text);
                return 1;
            }
    
            const char *type = json_string_value(json_object_get(root, "type"));
    
            if (type && strcmp(type, "list_users_response") == 0) {
                printf("\nUsuarios conectados:\n");
                json_t *user_list = json_object_get(root, "content");
    
                if (json_is_array(user_list)) {
                    size_t index;
                    json_t *value;
                    json_array_foreach(user_list, index, value) {
                        const char *usuario = json_string_value(value);
                        if (usuario){
                            printf("- %s\n", json_string_value(value));
                        }
                    }
                }
                awaiting_response = 0;
            }
            else if (type && strcmp(type, "user_info_response") == 0) {
                // Respuesta de la información de un usuario específico
                const char *target = json_string_value(json_object_get(root, "target"));
                json_t *content = json_object_get(root, "content");
                
                if (content && json_is_object(content)) {
                    const char *ip = json_string_value(json_object_get(content, "ip"));
                    const char *status = json_string_value(json_object_get(content, "status"));
            
                    printf("\nInformación del usuario %s:\n", target);
                    printf("IP: %s\n", ip ? ip : "No disponible");
                    printf("Estado: %s\n", status ? status : "No disponible");
                }
                else {
                    printf("Error: No se encontró información para el usuario %s\n", target);
                }
                awaiting_response = 0;
            }

            json_decref(root);
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
    { "chat-protocol", callback_client, 0, MAX_MESSAGE_LEN },
    { NULL, NULL, 0, 0 }
};


size_t mensaje_len;
unsigned char buffer[LWS_PRE + MAX_MESSAGE_LEN];

int main(int argc, char *argv[]) {

    if (argc != 4) {
        fprintf(stderr, "Llamar al cliente de esta forma:\n %s <nombredeusuario> <IPdelservidor> <puertodelservidor>\n", argv[0]);
        return 1;
    }
    
    strncpy(username, argv[1], MAX_NAME_LEN - 1);
    username[MAX_NAME_LEN - 1] = '\0';

    const char *server_ip = argv[2];
    int server_port = atoi(argv[3]);
    if (server_port <= 0) {
        fprintf(stderr, "Puerto inválido\n");
        return 1;
    }

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
    ccinfo.address = server_ip;
    ccinfo.port = server_port;
    ccinfo.path = "/";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "chat-protocol";

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "Error al conectar con el servidor WebSocket\n");
        return 1;
    }

    printf("Conectado al servidor %s en el puerto %d\n", server_ip, server_port);

    // Esperar a que la conexión se establezca
    while (!global_wsi) {
        lws_service(context, 0);
    }

    // -------------- MENÚ DE OPCIONES ------------------------
    int opcion;
    do {
        printf("\n--- MENÚ DE OPCIONES ---\n");
        printf("1. Chatear con todos los usuarios (broadcasting)\n");
        printf("2. Mensajes directos\n");
        printf("3. Cambiar de estado\n");
        printf("4. Usuarios conectados\n");
        printf("5. Información de un usuario\n");
        printf("6. Ayuda\n");
        printf("7. Salir\n");
        printf("Seleccione una opción: ");
        scanf("%d", &opcion);
        getchar();

        switch (opcion) {
            case 1:
                printf("Modo broadcast activado.\n");
                break;
            case 2:
                printf("Modo mensajes directos activado.\n");
                break;
            case 3:
                printf("\nSeleccione un nuevo estado:\n");
                printf("1. ACTIVO\n");
                printf("2. OCUPADO\n");
                printf("3. INACTIVO\n");
                printf("Seleccione una opción: ");
            
                int estado_opcion;
                scanf("%d", &estado_opcion);
                getchar(); // Limpiar buffer de entrada
            
                const char *nuevo_estado;
                switch (estado_opcion) {
                    case 1:
                        nuevo_estado = "ACTIVO";
                        break;
                    case 2:
                        nuevo_estado = "OCUPADO";
                        break;
                    case 3:
                        nuevo_estado = "INACTIVO";
                        break;
                    default:
                        printf("Opción inválida. Estado no cambiado.\n");
                        continue; // Regresa al menú
                }
            
                // Crear mensaje JSON para cambiar estado
                char json_status[MAX_MESSAGE_LEN];
                snprintf(json_status, sizeof(json_status),
                        "{\"type\": \"change_status\", \"sender\": \"%s\", \"content\": \"%s\"}",
                        username, nuevo_estado);
            
                // Enviar el mensaje al servidor
                mensaje_len = strlen(json_status);
                memcpy(&buffer[LWS_PRE], json_status, mensaje_len);
                
                lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
                
                printf("Estado cambiado a: %s\n", nuevo_estado);                
                break;
            case 4:
                // Crear mensaje JSON para solicitar la lista de usuarios
                char json_list_request[MAX_MESSAGE_LEN];
                snprintf(json_list_request, sizeof(json_list_request),
                        "{\"type\": \"list_users\", \"sender\": \"%s\"}", username);

                // Enviar la solicitud al servidor
                mensaje_len = strlen(json_list_request);
                memcpy(&buffer[LWS_PRE], json_list_request, mensaje_len);

                lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
                
                printf("Solicitando lista de usuarios...\n");
                awaiting_response = 1;
                
                while (awaiting_response) {
                    lws_service(context, 0);
                    usleep(100000);
                }

                break;
            case 5:
                printf("Ingrese el nombre del usuario: ");
                char target_user[MAX_NAME_LEN];
                fgets(target_user, sizeof(target_user), stdin);
                target_user[strcspn(target_user, "\n")] = 0;  // Eliminar salto de línea
            
                printf("Solicitando información sobre el usuario %s...\n", target_user);
            
                // Crear mensaje JSON para solicitar la información del usuario
                char json_user_info_request[MAX_MESSAGE_LEN];
                snprintf(json_user_info_request, sizeof(json_user_info_request),
                        "{\"type\": \"user_info\", \"sender\": \"%s\", \"target\": \"%s\"}",
                        username, target_user);
            
                // Enviar la solicitud al servidor
                mensaje_len = strlen(json_user_info_request);
                memcpy(&buffer[LWS_PRE], json_user_info_request, mensaje_len);
            
                lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
            
                awaiting_response = 1;  // Marcar como esperando respuesta
            
                // Esperar hasta que se reciba la respuesta del servidor
                while (awaiting_response) {
                    lws_service(context, 0);
                    usleep(100000);
                }
                break;
            case 6:
                printf("Ayuda: Elija una opción y siga las instrucciones.\n");
                break;
            case 7:
                printf("Saliendo del chat...\n");
                lws_context_destroy(context);
                return 0;
            default:
                printf("Opción inválida. Intente de nuevo.\n");
        }
    } while (opcion != 1 && opcion != 2);

    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);
    return 0;
}