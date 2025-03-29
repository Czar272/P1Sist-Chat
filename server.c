#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <jansson.h>
#include <time.h>
#include <pthread.h>

#define MAX_USERS 100

typedef struct {
    char username[32];
    struct lws *wsi;
    char status[16];
    char ip[64];
    int active;
} User;

static User users[MAX_USERS];
pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;

void gen_timestamp(char *buffer, size_t buffer_size) {
  time_t now = time(NULL);
  struct tm *t = gmtime(&now);
  strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", t);
}

static int callback_chat(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {

    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED: {
        printf("Cliente conectado\n");
        break;
    }

    case LWS_CALLBACK_RECEIVE: {
        // Copiamos el mensaje recibido a un buffer null-terminated
        char msg[2048];
        memset(msg, 0, sizeof(msg));
        if (len >= sizeof(msg)) len = sizeof(msg) - 1; // prevenir overflow
        memcpy(msg, in, len);
        msg[len] = '\0';

        printf("Mensaje recibido (%zu bytes): %s\n", len, msg);

        // Intentar parsear el mensaje como JSON
        json_error_t error;
        json_t *root = json_loads(msg, 0, &error);

        if (!root) {
            printf("Error al parsear JSON: %s\n", error.text);
            break;
        }

        // Extraer tipo y usuario emisor
        const char *type = json_string_value(json_object_get(root, "type"));
        const char *sender = json_string_value(json_object_get(root, "sender"));

        if (!type || !sender) {
            printf("Mensaje sin 'type' o 'sender'\n");
            json_decref(root);
            break;
        }

        if (strcmp(type, "register") == 0) {
            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i < MAX_USERS; i++) {
                if (!users[i].active) {
                    strcpy(users[i].username, sender);
                    users[i].wsi = wsi;
                    strcpy(users[i].status, "ACTIVO");

                    char rip[64];
                    char name[128];
                    lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi),
                                           name, sizeof(name), rip, sizeof(rip));
                    strcpy(users[i].ip, rip);
                    users[i].active = 1;

                    // Crear respuesta JSON simple
                    char timestamp[64];
                    gen_timestamp(timestamp, sizeof(timestamp));

                    // Crear arreglo de usuarios conectados
                    json_t *user_list = json_array();
                    for (int j = 0; j < MAX_USERS; j++) {
                        if (users[j].active) {
                            json_array_append_new(user_list, json_string(users[j].username));
                        }
                    }

                    // Crear objeto de respuesta
                    json_t *response = json_object();
                    json_object_set_new(response, "type", json_string("register_success"));
                    json_object_set_new(response, "sender", json_string("server"));
                    json_object_set_new(response, "content", json_string("Registro exitoso"));
                    json_object_set_new(response, "userList", user_list);
                    json_object_set_new(response, "timestamp", json_string(timestamp));

                    // Serializar a string
                    char *response_str = json_dumps(response, JSON_COMPACT);

                    // Enviar mensaje
                    unsigned char buf[LWS_PRE + 1024];
                    unsigned char *p = &buf[LWS_PRE];
                    size_t n = strlen(response_str);
                    memcpy(p, response_str, n);
                    lws_write(wsi, p, n, LWS_WRITE_TEXT);

                    // Liberar memoria
                    free(response_str);
                    json_decref(response);

                    break;
                }
            }
            pthread_mutex_unlock(&user_mutex);

        } else if (strcmp(type, "broadcast") == 0 ) {
            const char *content = json_string_value(json_object_get(root, "content"));
            char timestamp[64];
            gen_timestamp(timestamp, sizeof(timestamp));

            

            if (!content) {
                printf("Mensaje 'broadcast' inválido: falta 'content'\n");
                break;
            }
            // Reenviar a todos los usuarios activos
            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i < MAX_USERS; i++) {
                if (users[i].active && users[i].wsi) {

                    char response[512];
                    snprintf(response, sizeof(response),
                        "{\"type\":\"broadcast\",\"sender\":\"%s\","
                        "\"content\":\"%s\",\"timestamp\":\"%s\"}",
                        sender, content, timestamp);

                    unsigned char buf[LWS_PRE + 512];
                    unsigned char *p = &buf[LWS_PRE];
                    size_t n = strlen(response);
                    memcpy(p, response, n);
                    lws_write(users[i].wsi, p, n, LWS_WRITE_TEXT);
                }
            }
            pthread_mutex_unlock(&user_mutex);
            printf("Broadcast enviado por %s: %s\n", sender, content);

        } else if (strcmp(type, "private")==0) {

            const char *target = json_string_value(json_object_get(root, "target"));
            const char *content = json_string_value(json_object_get(root, "content"));

            if (!target || !content) {
                printf("Mensaje 'private' inválido\n");
                break;
            }

            // Buscar al usuario destino
            pthread_mutex_lock(&user_mutex);
            int encontrado = 0;
            for (int i = 0; i < MAX_USERS; i++) {
                if (users[i].active && strcmp(users[i].username, target) == 0) {
                    char response[512];
                    char timestamp[64];
                    gen_timestamp(timestamp, sizeof(timestamp));

                    snprintf(response, sizeof(response),
                        "{\"type\":\"private\",\"sender\":\"%s\",\"target\":\"%s\","  
                        "\"content\":\"%s\",\"timestamp\":\"%s\"}",
                        sender, target, content, timestamp);

                    unsigned char buf[LWS_PRE + 512];
                    unsigned char *p = &buf[LWS_PRE];
                    size_t n = strlen(response);
                    memcpy(p, response, n);
                    lws_write(users[i].wsi, p, n, LWS_WRITE_TEXT);

                    encontrado = 1;
                    printf("Mensaje privado de %s a %s: %s\n", sender, target, content);
                    break;
                }
            }
            pthread_mutex_unlock(&user_mutex);

            if (!encontrado) {
                printf("Usuario destino '%s' no encontrado o no conectado\n", target);
            }

        } else if (strcmp(type, "list_users") == 0) {
            char timestamp[64];
            gen_timestamp(timestamp, sizeof(timestamp));

            // Crear un arreglo JSON
            json_t *user_list = json_array();

            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i < MAX_USERS; i++) {
                if (users[i].active) {
                    json_array_append_new(user_list, json_string(users[i].username));
                }
            }
            pthread_mutex_unlock(&user_mutex);

            // Armar la respuesta
            json_t *response = json_object();
            json_object_set_new(response, "type", json_string("list_users_response"));
            json_object_set_new(response, "sender", json_string("server"));
            json_object_set_new(response, "content", user_list);
            json_object_set_new(response, "timestamp", json_string(timestamp));

            // Convertir JSON a string
            char *response_str = json_dumps(response, JSON_COMPACT);

            unsigned char buf[LWS_PRE + 1024];
            unsigned char *p = &buf[LWS_PRE];
            size_t n = strlen(response_str);
            memcpy(p, response_str, n);
            lws_write(wsi, p, n, LWS_WRITE_TEXT);

            printf("Lista de usuarios enviada a %s\n", sender);

            free(response_str);
            json_decref(response);

        } else if (strcmp(type, "user_info") == 0) {
            const char *target = json_string_value(json_object_get(root, "target"));

            if (!target) {
                printf("Mensaje 'user_info' inválido: falta 'target'\n");
                break;
            }

            int encontrado = 0;

            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i < MAX_USERS; i++) {
                if (users[i].active && strcmp(users[i].username, target) == 0) {
                    char timestamp[64];
                    gen_timestamp(timestamp, sizeof(timestamp));
        
                    // Construir contenido del mensaje
                    json_t *info = json_object();
                    json_object_set_new(info, "ip", json_string(users[i].ip));
                    json_object_set_new(info, "status", json_string(users[i].status));
        
                    json_t *response = json_object();
                    json_object_set_new(response, "type", json_string("user_info_response"));
                    json_object_set_new(response, "sender", json_string("server"));
                    json_object_set_new(response, "target", json_string(target));
                    json_object_set_new(response, "content", info);
                    json_object_set_new(response, "timestamp", json_string(timestamp));
        
                    // Serializar a string
                    char *response_str = json_dumps(response, JSON_COMPACT);
        
                    unsigned char buf[LWS_PRE + 1024];
                    unsigned char *p = &buf[LWS_PRE];
                    size_t n = strlen(response_str);
                    memcpy(p, response_str, n);
                    lws_write(wsi, p, n, LWS_WRITE_TEXT);
        
                    printf("Info enviada sobre %s\n", target);
        
                    free(response_str);
                    json_decref(response);
        
                    encontrado = 1;
                    break;
                }
            }

            pthread_mutex_unlock(&user_mutex);
            if (!encontrado) {
                printf("Usuario '%s' no encontrado\n", target);

            }
        
        } else if (strcmp(type, "change_status") == 0) {
            const char *new_status = json_string_value(json_object_get(root, "content"));

            if (!new_status) {
                printf("Mensaje 'change_status' inválido: falta 'content'\n");
                break;
            }
        
            int actualizado = 0;

            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i < MAX_USERS; i++) {
                if (users[i].active && strcmp(users[i].username, sender) == 0) {
                    strcpy(users[i].status, new_status);
                    actualizado = 1;
        
                    char timestamp[64];
                    gen_timestamp(timestamp, sizeof(timestamp));
        
                    // Contenido del mensaje
                    json_t *status_obj = json_object();
                    json_object_set_new(status_obj, "user", json_string(sender));
                    json_object_set_new(status_obj, "status", json_string(new_status));
        
                    // Mensaje completo
                    json_t *response = json_object();
                    json_object_set_new(response, "type", json_string("status_update"));
                    json_object_set_new(response, "sender", json_string("server"));
                    json_object_set_new(response, "content", status_obj);
                    json_object_set_new(response, "timestamp", json_string(timestamp));
        
                    char *response_str = json_dumps(response, JSON_COMPACT);
        
                    // Enviar a todos los usuarios conectados
                    for (int j = 0; j < MAX_USERS; j++) {
                        if (users[j].active && users[j].wsi) {
                            unsigned char buf[LWS_PRE + 1024];
                            unsigned char *p = &buf[LWS_PRE];
                            size_t n = strlen(response_str);
                            memcpy(p, response_str, n);
                            lws_write(users[j].wsi, p, n, LWS_WRITE_TEXT);
                        }
                    }
        
                    printf("Estado de %s cambiado a %s\n", sender, new_status);
        
                    free(response_str);
                    json_decref(response);
                    break;
                }
            }
            pthread_mutex_unlock(&user_mutex);
        
            if (!actualizado) {
                printf("Usuario %s no encontrado para actualizar estado\n", sender);

            }
        } else if (strcmp(type, "disconnect") == 0) {
            int encontrado = 0;
        
            pthread_mutex_lock(&user_mutex);
            for (int i = 0; i< MAX_USERS; i++) {
                if (users[i].active && strcmp(users[i].username, sender) == 0 ){
                    users[i].active = 0;
                    users[i].wsi = NULL;
        
                    char timestamp[64];
                    gen_timestamp(timestamp, sizeof(timestamp));
        
                    char message[256];
                    snprintf(message, sizeof(message), "%s ha salido", sender);
        
                    // Armar respuesta
                    json_t *response = json_object();
                    json_object_set_new(response, "type", json_string("user_disconnected"));
                    json_object_set_new(response, "sender", json_string("server"));
                    json_object_set_new(response, "content", json_string(message));
                    json_object_set_new(response, "timestamp", json_string(timestamp));
        
                    char *response_str = json_dumps(response, JSON_COMPACT);
        
                    // Enviar a todos los usuarios conectados
                    for (int j = 0; j < MAX_USERS; j++) {
                        if (users[j].active && users[j].wsi) {
                            unsigned char buf[LWS_PRE + 512];
                            unsigned char *p = &buf[LWS_PRE];
                            size_t n = strlen(response_str);
                            memcpy(p, response_str, n);
                            lws_write(users[j].wsi, p, n, LWS_WRITE_TEXT);
                        }
                    }
        
                    printf("Usuario %s se desconectó voluntariamente\n", sender);
        
                    free(response_str);
                    json_decref(response);
                    encontrado = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&user_mutex);
        
            if (!encontrado) {
                printf("Usuario %s no encontrado para desconexión\n", sender);
            }
        
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, (unsigned char *)"Bye", 3);
            return -1;
        }
        json_decref(root);
        break;
    }

    case LWS_CALLBACK_CLOSED: {
        pthread_mutex_lock(&user_mutex);
        for (int i = 0; i < MAX_USERS; i++) {
            if (users[i].wsi == wsi) {
                printf("Usuario %s se desconectó\n", users[i].username);
                users[i].active = 0;
                break;
            }
        }
        pthread_mutex_unlock(&user_mutex);
        break;
    }

    default:
        break;
    }

    return 0;
}

static struct lws_protocols protocols[] = {
    {
        .name = "chat-protocol",
        .callback = callback_chat,
        .per_session_data_size = 0,
        .rx_buffer_size = 0,
    },
    { NULL, NULL, 0, 0 }
};

int main(int argc, char *argv[]) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    if (argc != 2) {
        printf("Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    int puerto = atoi(argv[1]);
    if (puerto <= 0 || puerto > 65535) {
        printf("Error: Puerto inválido. Debe estar entre 1 y 65535.\n");
        return 1;
    }

    printf("Servidor iniciado en el puerto: %d\n", puerto);

    info.port = puerto;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Error al crear contexto\n");
        return 1;
    }

    printf("Servidor WebSocket activo\n");

    while (1)
        lws_service(context, 1000);

    lws_context_destroy(context);
    return 0;
}
