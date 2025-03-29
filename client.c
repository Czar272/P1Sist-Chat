// gcc client.c -o client -lwebsockets -ljansson
// npx wscat -c ws://localhost:8000

#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jansson.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_MESSAGE_LEN 512
#define MAX_PRIVATE_MESSAGES 100
#define MAX_NAME_LEN 50
#define MAX_BROADCAST_MESSAGES 10

typedef struct {
    char sender[MAX_MESSAGE_LEN];
    char content[MAX_MESSAGE_LEN];
    char target[MAX_MESSAGE_LEN];
} private_message_t;

private_message_t private_messages[MAX_PRIVATE_MESSAGES];
int private_message_count = 0;
char broadcast_messages[MAX_BROADCAST_MESSAGES][MAX_MESSAGE_LEN];
int broadcast_count = 0;
int in_broadcast_mode = 0;
int in_private_chat = 0;
char current_private_chat[MAX_NAME_LEN] = "";

static struct lws *global_wsi;
static char username[MAX_NAME_LEN];
static int awaiting_response = 0;
int is_writing = 0;
pthread_mutex_t writing_mutex = PTHREAD_MUTEX_INITIALIZER;

// Función para leer un carácter sin esperar Enter
char getch() {
    struct termios oldt, newt;
    char ch;
    tcgetattr(STDIN_FILENO, &oldt); // Obtener atributos actuales del terminal
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Desactivar modo canónico y eco
    tcsetattr(STDIN_FILENO, TCSANOW, &newt); // Aplicar cambios

    ch = getchar(); // Leer un solo carácter

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restaurar configuración original
    return ch;
}

void redraw_broadcast_screen() {
    system("clear");

    // Imprimir los últimos mensajes de broadcast
    for (int i = 0; i < broadcast_count; i++) {
        printf("%s\n", broadcast_messages[i]);
    }
    printf("\nModo broadcast activado. Presiona ESC para volver al menú.\n");
}

void redraw_private_chat_screen() {
    system("clear");

    // Mostrar los mensajes privados
    for (int i = 0; i < private_message_count; i++) {
        if (strcmp(private_messages[i].sender, current_private_chat) == 0 || (strcmp(private_messages[i].sender, username) == 0 && strcmp(private_messages[i].target, current_private_chat) == 0)) {
            printf("[Privado] %s: %s\n", private_messages[i].sender, private_messages[i].content);
        }
    }

    printf("\nChateando con %s. Presiona ESC para volver al menú.\n", current_private_chat);
}

// Función para manejar la recepción de mensajes
void *receive_messages(void *arg) {
    struct lws_context *context = (struct lws_context *)arg;
    while (1) {
        pthread_mutex_lock(&writing_mutex);
        if (!is_writing) {
            lws_service(context, 0);
        }
        pthread_mutex_unlock(&writing_mutex);
        usleep(100000); // Pequeño delay para evitar consumir mucho CPU
    }
    return NULL;
}

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

        case LWS_CALLBACK_CLIENT_RECEIVE:
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
        
            if (!awaiting_response) {

                if (type && strcmp(type, "broadcast") == 0) {

                    const char *sender = json_string_value(json_object_get(root, "sender"));
                    const char *content = json_string_value(json_object_get(root, "content"));
            
                    if (sender && content) {
                        // Guardar mensaje en un buffer global para mostrarlo luego
                        char formatted_message[MAX_MESSAGE_LEN];
                        snprintf(formatted_message, sizeof(formatted_message), "%s: %s", sender, content);
            
                        // Agregar el mensaje al historial de mensajes
                        if (broadcast_count < MAX_BROADCAST_MESSAGES) {
                            strcpy(broadcast_messages[broadcast_count], formatted_message);
                            broadcast_count++;
                        } else {
                            // Desplazar mensajes antiguos para hacer espacio
                            for (int i = 1; i < MAX_BROADCAST_MESSAGES; i++) {
                                strcpy(broadcast_messages[i - 1], broadcast_messages[i]);
                            }
                            strcpy(broadcast_messages[MAX_BROADCAST_MESSAGES - 1], formatted_message);
                        }
            
                        // Volver a dibujar la pantalla en modo broadcast
                        if (in_broadcast_mode) {
                            redraw_broadcast_screen();
                        }
                    }
                }
                else if (type && strcmp(type, "private") == 0) {
                    
                    const char *sender = json_string_value(json_object_get(root, "sender"));
                    const char *target = json_string_value(json_object_get(root, "target"));
                    const char *content = json_string_value(json_object_get(root, "content"));
            
                    if (sender && target && content) {
                        // Guardar mensaje en el historial de mensajes privados
                        if (private_message_count < MAX_PRIVATE_MESSAGES) {
                            snprintf(private_messages[private_message_count].sender, MAX_MESSAGE_LEN, "%s", sender);
                            snprintf(private_messages[private_message_count].content, MAX_MESSAGE_LEN, "%s", content);
                            snprintf(private_messages[private_message_count].target, MAX_MESSAGE_LEN, "%s", target);
                            private_message_count++;
                        }
            
                        // Mostrar el mensaje solo si estamos en el chat privado con ese usuario
                        if (in_private_chat && strcmp(current_private_chat, sender) == 0) {
                            redraw_private_chat_screen();
                        } else {
                            printf("\nNuevo mensaje privado de %s: %s\n", sender, content);
                        }
                    }
                }
            } else {

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

    pthread_t receive_thread;
    pthread_create(&receive_thread, NULL, receive_messages, (void *)context);

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
                in_broadcast_mode = 1;
                redraw_broadcast_screen();

                while (1) {
                    char mensaje_usuario[MAX_MESSAGE_LEN] = {0};

                    char c = getch();
                    if (c == 27) { // 27 es el código ASCII de ESC
                        printf("\nSaliendo del modo broadcast...\n");
                        in_broadcast_mode = 0;
                        break;
                    }

                    pthread_mutex_lock(&writing_mutex);
                    is_writing = 1;
                    pthread_mutex_unlock(&writing_mutex);

                    // Leer el mensaje después de la primera tecla presionada
                    ungetc(c, stdin);
                    fgets(mensaje_usuario, sizeof(mensaje_usuario), stdin);
                    mensaje_usuario[strcspn(mensaje_usuario, "\n")] = 0; // Eliminar salto de línea

                    // No activar si no escribió nada
                    if (strlen(mensaje_usuario) == 0) {
                        pthread_mutex_lock(&writing_mutex);
                        is_writing = 0;
                        pthread_mutex_unlock(&writing_mutex);
                        continue;
                    }

                    // Crear JSON {"type": "broadcast", "sender": "usuario", "mensaje": "texto"}
                    char json_mensaje[MAX_MESSAGE_LEN];
                    snprintf(json_mensaje, sizeof(json_mensaje), 
                        "{\"type\": \"broadcast\", \"sender\": \"%s\", \"content\": \"%s\"}",
                        username, mensaje_usuario);

                    // Enviar mensaje al servidor
                    mensaje_len = strlen(json_mensaje);
                    memcpy(&buffer[LWS_PRE], json_mensaje, mensaje_len);
                    lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);

                    pthread_mutex_lock(&writing_mutex);
                    is_writing = 0;
                    pthread_mutex_unlock(&writing_mutex);

                    lws_service(context, 0);
                }
                break;

            case 2:
                in_private_chat = 1;
                printf("Ingrese el nombre del usuario con el que desea chatear: ");
                fgets(current_private_chat, sizeof(current_private_chat), stdin);
                current_private_chat[strcspn(current_private_chat, "\n")] = 0;
            
                // Limpiar mensajes anteriores
                system("clear");
                redraw_private_chat_screen();

                while (1) {
                    char mensaje_usuario[MAX_MESSAGE_LEN] = {0};
            
                    char c = getch();
                    if (c == 27) { // ESC para salir
                        printf("\nSaliendo del chat privado con %s...\n", current_private_chat);
                        in_private_chat = 0;
                        break;
                    }
            
                    // Si la primera tecla no es ESC, activar is_writing
                    pthread_mutex_lock(&writing_mutex);
                    is_writing = 1;
                    pthread_mutex_unlock(&writing_mutex);
            
                    // Leer el mensaje después de la primera tecla presionada
                    ungetc(c, stdin);
                    fgets(mensaje_usuario, sizeof(mensaje_usuario), stdin);
                    mensaje_usuario[strcspn(mensaje_usuario, "\n")] = 0;
            
                    if (strlen(mensaje_usuario) == 0) { 
                        pthread_mutex_lock(&writing_mutex);
                        is_writing = 0; // No activar si no escribió nada
                        pthread_mutex_unlock(&writing_mutex);
                        continue;
                    }
            
                    // Crear JSON {"type": "private", "sender": "usuario", "target": "destino", "content": "mensaje"}
                    char json_mensaje[MAX_MESSAGE_LEN];
                    snprintf(json_mensaje, sizeof(json_mensaje), 
                        "{\"type\": \"private\", \"sender\": \"%s\", \"target\": \"%s\", \"content\": \"%s\"}",
                        username, current_private_chat, mensaje_usuario);
            
                    // Enviar mensaje al servidor
                    mensaje_len = strlen(json_mensaje);
                    memcpy(&buffer[LWS_PRE], json_mensaje, mensaje_len);

                    if (private_message_count < MAX_PRIVATE_MESSAGES) {
                        snprintf(private_messages[private_message_count].sender, MAX_MESSAGE_LEN, "%s", username);
                        snprintf(private_messages[private_message_count].content, MAX_MESSAGE_LEN, "%s", mensaje_usuario);
                        snprintf(private_messages[private_message_count].target, MAX_MESSAGE_LEN, "%s", current_private_chat);
                        private_message_count++;
                    }
                    
                    // Refrescar la pantalla para que el remitente vea su propio mensaje
                    redraw_private_chat_screen();

                    lws_write(wsi, &buffer[LWS_PRE], mensaje_len, LWS_WRITE_TEXT);
                    lws_service(context, 0);
            
                    // Desactivar is_writing después de enviar el mensaje
                    pthread_mutex_lock(&writing_mutex);
                    is_writing = 0;
                    pthread_mutex_unlock(&writing_mutex);
                }
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
                char target_user_info[MAX_NAME_LEN];
                fgets(target_user_info, sizeof(target_user_info), stdin);
                target_user_info[strcspn(target_user_info, "\n")] = 0;  // Eliminar salto de línea
            
                printf("Solicitando información sobre el usuario %s...\n", target_user_info);
            
                // Crear mensaje JSON para solicitar la información del usuario
                char json_user_info_request[MAX_MESSAGE_LEN];
                snprintf(json_user_info_request, sizeof(json_user_info_request),
                        "{\"type\": \"user_info\", \"sender\": \"%s\", \"target\": \"%s\"}",
                        username, target_user_info);
            
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
    } while (opcion != 7);

    pthread_cancel(receive_thread);
    pthread_join(receive_thread, NULL);

    lws_context_destroy(context);

    return 0;
}