// =============================================================
// FILE: components/ems/ems_cmd.c  (EIS wired)
// =============================================================
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "driver/uart.h"

#include "ems/ems_uart.h"
#include "ems/ems_eis.h"   // <— EIS integration
#include "telemetry/mqtt_bus.h"
#include "config/config.h"

#ifndef ESP_RETURN_ON_ERROR
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { \
    esp_err_t __err__ = (x); \
    if (__err__ != ESP_OK) { \
        ESP_LOGE(tag, "%s: %s", msg, esp_err_to_name(__err__)); \
        return __err__; \
    } \
} while(0)
#endif

static const char *TAG = "EMS_CMD";
static esp_mqtt_client_handle_t s_client;
static char s_topic_cmd[128];
static char s_topic_status[128];

#ifndef CONFIG_EMS_LINE_TIMEOUT_MS
#define CONFIG_EMS_LINE_TIMEOUT_MS 1000
#endif
#ifndef CONFIG_EMS_IDLE_TIMEOUT_MS
#define CONFIG_EMS_IDLE_TIMEOUT_MS 5000
#endif

// ---------- utils ----------
static void publish_json(const char *topic, cJSON *obj){ if(!topic||!obj||!s_client) return; char *p=cJSON_PrintUnformatted(obj); if(!p) return; esp_mqtt_client_publish(s_client, topic, p, 0, 1, false); cJSON_free(p); }
static size_t trim_crlf(char *s){ if(!s) return 0; size_t n=strlen(s); while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]='\0'; return n; }

// Map UTF‑8 smart chars → ASCII; drop CR. (why: Pico expects LF‑only ASCII)
static void sanitize_line_inplace(char *s){ if(!s) return; char *in=s,*out=s; while(*in){ unsigned char c=(unsigned char)*in; if(c=='\r'){in++;continue;} if(c=='\t'||(c>=0x20&&c<=0x7E)){*out++=*in++;continue;} if((unsigned char)in[0]==0xE2&&(unsigned char)in[1]==0x88&&(unsigned char)in[2]==0x92){*out='-';out++;in+=3;continue;} if((unsigned char)in[0]==0xE2&&(unsigned char)in[1]==0x80&&((unsigned char)in[2]==0x93||(unsigned char)in[2]==0x94)){*out='-';out++;in+=3;continue;} if((unsigned char)in[0]==0xC2&&(unsigned char)in[1]==0xA0){*out=' ';out++;in+=2;continue;} if((unsigned char)in[0]==0xE2&&(unsigned char)in[1]==0x80&&((unsigned char)in[2]==0x9C||(unsigned char)in[2]==0x9D)){*out='"';out++;in+=3;continue;} in++; } *out='\0'; }

// Strip PSTrace header: leading single‑line "e"/"l".
static const char *strip_pstrace_header(const char *script){ if(!script) return NULL; const char *nl=strchr(script,'\n'); size_t first= nl? (size_t)(nl-script) : strlen(script); size_t i=0; while(i<first&&(script[i]=='\r'||script[i]==' '||script[i]=='\t')) i++; size_t j=first; while(j>i&&(script[j-1]=='\r'||script[j-1]==' '||script[j-1]=='\t')) j--; bool only_e = (j-i==1)&&(script[i]=='e'||script[i]=='E'); bool only_l=(j-i==1)&&(script[i]=='l'||script[i]=='L'); return (only_e||only_l)? (nl? nl+1 : script+first) : script; }

static void publish_ms_line(int seq, const char *line){ cJSON *m=cJSON_CreateObject(); cJSON_AddStringToObject(m,"op","ms_line"); cJSON_AddNumberToObject(m,"seq",seq); cJSON_AddStringToObject(m,"line", line?line:""); publish_json(s_topic_status,m); cJSON_Delete(m);} 
static void finish_with_result(bool ok, const char *err, const char *code){ cJSON *m=cJSON_CreateObject(); cJSON_AddStringToObject(m,"op","ms_result"); cJSON_AddBoolToObject(m,"ok",ok); if(!ok&&err) cJSON_AddStringToObject(m,"err",err); if(!ok&&code) cJSON_AddStringToObject(m,"code",code); publish_json(s_topic_status,m); cJSON_Delete(m);} 

// I/O primitives ----------------------------------------------
esp_err_t ems_send_str(const char *s, bool append_lf, int tx_timeout_ms);
int       ems_read_until(char *out, size_t out_cap, const char *delims, int timeout_ms);

// Send cmd+LF, each script line+LF (LF‑only ASCII), then empty‑line terminator.
static esp_err_t send_script_with_terminator(const char *cmd, const char *script_in)
{
    if(!cmd||!script_in) return ESP_ERR_INVALID_ARG;
    const char *script = strip_pstrace_header(script_in);
    ESP_RETURN_ON_ERROR(ems_send_str(cmd, true, 50), TAG, "cmd");
    const char *p = script;
    while(*p){ const char *nl=strchr(p,'\n'); size_t len = nl? (size_t)(nl-p) : strlen(p); char line[256]; if(len>=sizeof line) return ESP_ERR_NO_MEM; memcpy(line,p,len); line[len]='\0'; sanitize_line_inplace(line); if(line[0] != '\0'){ ESP_RETURN_ON_ERROR(ems_send_str(line, true, 50), TAG, "line"); } p = nl? nl+1 : p+len; }
    ESP_RETURN_ON_ERROR(ems_send_str("", true, 50), TAG, "term"); // empty line = end
    return ESP_OK;
}

// ---------- ops ----------
static void handle_probe(cJSON *root){ (void)root; char ver[128]; esp_err_t r=ems_probe(ver,sizeof ver, CONFIG_EMS_LINE_TIMEOUT_MS); cJSON *out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"op","probe"); if(r==ESP_OK){ cJSON_AddBoolToObject(out,"ok",true); cJSON_AddStringToObject(out,"version",ver);} else { cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err", (r==ESP_ERR_TIMEOUT)?"timeout":"io_fail"); } publish_json(s_topic_status,out); cJSON_Delete(out);} 

static void handle_send(cJSON *root){ const cJSON *line=cJSON_GetObjectItemCaseSensitive(root,"line"); int wait_ms=500; const cJSON *wait=cJSON_GetObjectItemCaseSensitive(root,"wait_ms"); if(cJSON_IsNumber(wait)&&wait->valuedouble>=0) wait_ms=(int)wait->valuedouble; cJSON *out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"op","send"); if(!cJSON_IsString(line)||!line->valuestring){ cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err","missing_line"); publish_json(s_topic_status,out); cJSON_Delete(out); return;} char buf[256]; ems_io_lock(); uart_flush_input(CONFIG_APP_EMS_UART_NUM); esp_err_t wr=ems_send_str(line->valuestring,true,50); int rd=-1; if(wr==ESP_OK) rd=ems_read_until(buf,sizeof buf,"\n",wait_ms); ems_io_unlock(); if(wr==ESP_OK && rd>=0){ trim_crlf(buf); cJSON_AddBoolToObject(out,"ok",true); cJSON_AddStringToObject(out,"resp",buf);} else { cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err", (wr!=ESP_OK)?"write_fail":"read_timeout"); } publish_json(s_topic_status,out); cJSON_Delete(out);} 

static void handle_ms_e(cJSON *root){ const cJSON *script=cJSON_GetObjectItemCaseSensitive(root,"script"); int line_to=CONFIG_EMS_LINE_TIMEOUT_MS, idle_to=CONFIG_EMS_IDLE_TIMEOUT_MS; const cJSON *lt=cJSON_GetObjectItemCaseSensitive(root,"line_timeout_ms"); const cJSON *it=cJSON_GetObjectItemCaseSensitive(root,"idle_timeout_ms"); if(cJSON_IsNumber(lt)&&lt->valuedouble>=0) line_to=(int)lt->valuedouble; if(cJSON_IsNumber(it)&&it->valuedouble>=0) idle_to=(int)it->valuedouble; cJSON *start=cJSON_CreateObject(); cJSON_AddStringToObject(start,"op","ms_start"); publish_json(s_topic_status,start); cJSON_Delete(start); if(!cJSON_IsString(script)||!script->valuestring){ finish_with_result(false,"missing_script",NULL); return;} char buf[512]; int seq=0; bool error=false, echo=false; ems_io_lock(); uart_flush_input(CONFIG_APP_EMS_UART_NUM); esp_err_t wr=send_script_with_terminator("e", script->valuestring); int idle_left=idle_to; while(wr==ESP_OK && idle_left>=0){ int rd=ems_read_until(buf,sizeof buf,"\n",line_to); if(rd<0){ idle_left-=line_to; continue;} idle_left=idle_to; trim_crlf(buf); if (!echo && buf[0] == 'e') { memmove(buf, buf + 1, strlen(buf)); echo = true; if (buf[0] == '\0') continue; } if(buf[0]=='!'){ publish_ms_line(seq++,buf); error=true; break;} if(buf[0]=='\0') break; publish_ms_line(seq++,buf);} ems_io_unlock(); if(wr!=ESP_OK) finish_with_result(false,"write_fail",NULL); else if(error) finish_with_result(false,"ms_error",buf+1); else if(idle_left<0) finish_with_result(false,"idle_timeout",NULL); else finish_with_result(true,NULL,NULL);} 

static void handle_ms_l(cJSON *root){ const cJSON *script=cJSON_GetObjectItemCaseSensitive(root,"script"); cJSON *out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"op","ms_l"); if(!cJSON_IsString(script)||!script->valuestring){ cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err","missing_script"); publish_json(s_topic_status,out); cJSON_Delete(out); return;} ems_io_lock(); uart_flush_input(CONFIG_APP_EMS_UART_NUM); esp_err_t wr=send_script_with_terminator("l", script->valuestring); char ack[16]; int rd=(wr==ESP_OK)? ems_read_until(ack,sizeof ack,"\n",CONFIG_EMS_LINE_TIMEOUT_MS) : -1; ems_io_unlock(); if(wr==ESP_OK && rd>0){ trim_crlf(ack); bool ok=(strcmp(ack,"l")==0); cJSON_AddBoolToObject(out,"ok",ok); if(!ok) cJSON_AddStringToObject(out,"err",ack);} else { cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err", (wr!=ESP_OK)?"write_fail":"read_timeout"); } publish_json(s_topic_status,out); cJSON_Delete(out);} 

static void handle_ms_r(cJSON *root){ (void)root; cJSON *start=cJSON_CreateObject(); cJSON_AddStringToObject(start,"op","ms_start"); publish_json(s_topic_status,start); cJSON_Delete(start); char buf[512]; int seq=0; bool error=false, echo=false; int line_to=CONFIG_EMS_LINE_TIMEOUT_MS, idle_to=CONFIG_EMS_IDLE_TIMEOUT_MS; ems_io_lock(); uart_flush_input(CONFIG_APP_EMS_UART_NUM); esp_err_t wr=ems_send_str("r", true, 50); int idle_left=idle_to; while(wr==ESP_OK && idle_left>=0){ int rd=ems_read_until(buf,sizeof buf,"\n",line_to); if(rd<0){ idle_left-=line_to; continue;} idle_left=idle_to; trim_crlf(buf); if (!echo && buf[0] == 'r') { memmove(buf, buf + 1, strlen(buf)); echo = true; if (buf[0] == '\0') continue; } if(buf[0]=='!'){ publish_ms_line(seq++,buf); error=true; break;} if(buf[0]=='\0') break; publish_ms_line(seq++,buf);} ems_io_unlock(); if(wr!=ESP_OK) finish_with_result(false,"write_fail",NULL); else if(error) finish_with_result(false,"ms_error",buf+1); else if(idle_left<0) finish_with_result(false,"idle_timeout",NULL); else finish_with_result(true,NULL,NULL);} 

static void handle_ms_ctrl(cJSON *root){ const cJSON *cmd=cJSON_GetObjectItemCaseSensitive(root,"cmd"); cJSON *out=cJSON_CreateObject(); cJSON_AddStringToObject(out,"op","ms_ctrl"); if(!cJSON_IsString(cmd)||!cmd->valuestring||strlen(cmd->valuestring)!=1){ cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err","cmd must be one of h,H,Z,Y"); publish_json(s_topic_status,out); cJSON_Delete(out); return;} char c=cmd->valuestring[0]; if(!(c=='h'||c=='H'||c=='Z'||c=='Y')){ cJSON_AddBoolToObject(out,"ok",false); cJSON_AddStringToObject(out,"err","unsupported_cmd"); publish_json(s_topic_status,out); cJSON_Delete(out); return;} char s[2]={c,0}; ems_io_lock(); esp_err_t wr=ems_send_str(s, true, 50); ems_io_unlock(); cJSON_AddBoolToObject(out,"ok", wr==ESP_OK); if(wr!=ESP_OK) cJSON_AddStringToObject(out,"err","write_fail"); publish_json(s_topic_status,out); cJSON_Delete(out);} 

// ---------- mqtt glue ----------
static void mqtt_evt(void *handler_args, esp_event_base_t base, int32_t eid, void *event_data)
{ (void)handler_args; (void)base; (void)eid; esp_mqtt_event_handle_t e = event_data; switch(e->event_id){ case MQTT_EVENT_CONNECTED: ESP_LOGI(TAG,"connected → subscribing %s", s_topic_cmd); esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1); break; case MQTT_EVENT_DATA: if(!e->topic||!e->data) break; if (strncmp(e->topic, s_topic_cmd, e->topic_len) == 0 && strlen(s_topic_cmd) == (size_t)e->topic_len) { cJSON *root=cJSON_ParseWithLength(e->data, e->data_len); if(!root) break; 
                // ---- EIS first: let EIS handler consume if relevant
                if (ems_eis_handle_mqtt(root) != ESP_ERR_NOT_FOUND) { cJSON_Delete(root); break; }

                const cJSON *op=cJSON_GetObjectItemCaseSensitive(root,"op"); if(cJSON_IsString(op)&&op->valuestring){ if      (strcmp(op->valuestring,"probe")==0) handle_probe(root); else if (strcmp(op->valuestring,"send")==0) handle_send(root); else if (strcmp(op->valuestring,"ms_e")==0 || strcmp(op->valuestring,"ms.run")==0) handle_ms_e(root); else if (strcmp(op->valuestring,"ms_l")==0 || strcmp(op->valuestring,"ms.load")==0) handle_ms_l(root); else if (strcmp(op->valuestring,"ms_r")==0 || strcmp(op->valuestring,"ms.run_loaded")==0) handle_ms_r(root); else if (strcmp(op->valuestring,"ms_ctrl")==0) handle_ms_ctrl(root);} cJSON_Delete(root);} break; default: break; }}

static void make_topics(void){ char id[64]={0}; if (config_get_device_id(id,sizeof id) != ESP_OK || id[0]=='\0') snprintf(id,sizeof id,"unknown"); snprintf(s_topic_cmd, sizeof s_topic_cmd, "devices/%s/commands/ems", id); snprintf(s_topic_status, sizeof s_topic_status, "devices/%s/status/ems", id); }

esp_err_t ems_cmd_init(void)
{
    make_topics();
    ESP_RETURN_ON_ERROR(ems_uart_init(), TAG, "uart");
    ESP_RETURN_ON_ERROR(telemetry_mqtt_ensure_started(), TAG, "mqtt");
    s_client = telemetry_mqtt_get_client();
    // init EIS once; it will publish on the same status topic
    ESP_ERROR_CHECK_WITHOUT_ABORT(ems_eis_init());

    if (!s_client) {
        ESP_LOGW(TAG, "MQTT client not ready yet; call ems_cmd_attach_mqtt() later.");
        return ESP_OK;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    ESP_LOGI(TAG, "ems_cmd ready; cmd=%s status=%s", s_topic_cmd, s_topic_status);
    return ESP_OK;
}

esp_err_t ems_cmd_attach_mqtt(void)
{
    esp_mqtt_client_handle_t c = telemetry_mqtt_get_client();
    if (!c) return ESP_ERR_INVALID_STATE;
    if (s_client != c) s_client = c;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    ESP_LOGI(TAG, "ems_cmd attached to mqtt; cmd=%s status=%s", s_topic_cmd, s_topic_status);
    return ESP_OK;
}
