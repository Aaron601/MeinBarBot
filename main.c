#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <sqlite3.h>
#include <curl/curl.h>

#define PORT 5000
#define ESP32_URL "http://192.168.1.100/set-positions" // IP des ESP32 anpassen (oder Wokwi IP)
#define DB_FILE "barbot.db"

// Hilfsfunktion: Sendet den JSON-Payload per HTTP-POST an den ESP32
int send_to_esp32(const char *json_payload) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, ESP32_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

// Holt die Schienenpositionen für eine CocktailID aus der Datenbank
char* get_targets_json(int cocktail_id) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    
    if (sqlite3_open(DB_FILE, &db) != SQLITE_OK) {
        fprintf(stderr, "DB Fehler: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    // SQL: SchienenPos sortiert nach RezeptPos holen
    const char *sql = 
        "SELECT z.SchienenPos "
        "FROM Rezept r "
        "JOIN Zutat z ON r.ZutatID = z.ZutatID "
        "WHERE r.CocktailID = ? "
        "ORDER BY r.RezeptPos ASC;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_bind_int(stmt, 1, cocktail_id);

    // Dynamic Buffer für das JSON-Array bauen
    char *json_buf = malloc(512);
    strcpy(json_buf, "{\"targets\":[");

    int first = 1;
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int pos = sqlite3_column_int(stmt, 0);
        if (!first) {
            strcat(json_buf, ",");
        }
        char pos_str[16];
        snprintf(pos_str, sizeof(pos_str), "%d", pos);
        strcat(json_buf, pos_str);
        first = 0;
        count++;
    }

    strcat(json_buf, "]}");

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (count == 0) {
        free(json_buf);
        return NULL; // Keine Positionen gefunden (ID ungültig)
    }

    return json_buf;
}

// Handler für eingehende HTTP-Anfragen
static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                           const char *url, const char *method,
                                           const char *version, const char *upload_data,
                                           size_t *upload_data_size, void **con_cls) {
    
    // Einfache HTML-Antwort
    const char *resp_ok = "{\"status\":\"ok\"}";
    const char *resp_err = "{\"status\":\"error\"}";
    struct MHD_Response *response;
    enum MHD_Result ret;

    // Route: /order?id=X
    if (strcmp(url, "/order") == 0) {
        const char *id_param = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "id");
        
        if (!id_param) {
            response = MHD_create_response_from_buffer(strlen(resp_err), (void*)resp_err, MHD_RESPMEM_PERSISTENT);
            return MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, response);
        }

        int cocktail_id = atoi(id_param);
        printf("[INFO] Bestellung empfangen fuer Cocktail ID: %d\n", cocktail_id);

        char *json_targets = get_targets_json(cocktail_id);

        if (!json_targets) {
            printf("[ERROR] Cocktail ID %d nicht gefunden oder keine Zutaten.\n", cocktail_id);
            response = MHD_create_response_from_buffer(strlen(resp_err), (void*)resp_err, MHD_RESPMEM_PERSISTENT);
            return MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        }

        printf("[INFO] Generierter ESP32-Payload: %s\n", json_targets);

        // An ESP32 senden
        int esp_res = send_to_esp32(json_targets);
        free(json_targets);

        if (esp_res == 0) {
            printf("[INFO] Erfolgreich an ESP32 gesendet!\n");
            response = MHD_create_response_from_buffer(strlen(resp_ok), (void*)resp_ok, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        } else {
            printf("[ERROR] Fehler beim Senden an ESP32!\n");
            response = MHD_create_response_from_buffer(strlen(resp_err), (void*)resp_err, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        }

        MHD_destroy_response(response);
        return ret;
    }

    // Default 404
    response = MHD_create_response_from_buffer(strlen(resp_err), (void*)resp_err, MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return ret;
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        PORT,
        NULL, NULL,
        &answer_to_connection, NULL,
        MHD_OPTION_END
    );

    if (!daemon) {
        fprintf(stderr, "Konnte HTTP-Server auf Port %d nicht starten.\n", PORT);
        return 1;
    }

    printf("BarBot C-Backend laeuft auf Port %d...\n", PORT);
    printf("Druecke ENTER zum Beenden.\n");
    getchar();

    MHD_stop_daemon(daemon);
    curl_global_cleanup();

    return 0;
}