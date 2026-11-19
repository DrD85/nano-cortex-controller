#include <assert.h>
#include <string.h>
#include "midi_switches.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdio.h>
static int64_t now; static void (*tcb)(void*); static int64_t tdue = -1;
int64_t esp_timer_get_time(void){return now;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h){tcb=a->callback;*h=(void*)1;return 0;}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us){tdue=now+(int64_t)us;return 0;}
esp_err_t esp_timer_stop(esp_timer_handle_t h){tdue=-1;return 0;}
static unsigned char store[64]; static size_t store_len; static int saves;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h){*h=1;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len){if(!store_len)return 1;if(*len<store_len)return 1;memcpy(v,store,store_len);*len=store_len;return 0;}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len){memcpy(store,v,len);store_len=len;saves++;return 0;}
esp_err_t nvs_commit(nvs_handle_t h){return 0;}
void nvs_close(nvs_handle_t h){}
static char log_[512];
static void ev(int n, footswitch_event_t e){char b[16];snprintf(b,16,"%c%d ","PRH"[e],n);strcat(log_,b);}
static int ln=-1, lo=-1;
static void learned(int n,int o){ln=n;lo=o;}
static void advance(int64_t us){int64_t end=now+us;while(tdue>=0&&tdue<=end){now=tdue;tdue=-1;tcb(NULL);}now=end;}
#define CHECK(s) do{ if(strcmp(log_,s)){printf("line %d: got '%s' want '%s'\n",__LINE__,log_,s);return 1;} log_[0]=0; }while(0)
int main(void){
  midi_switches_start(ev, learned);
  assert(!midi_switches_take(0xB0,20,127));            // nothing learned
  midi_switches_learn(3);
  assert(!midi_switches_take(0xB0,1,127));             // expression never
  assert(!midi_switches_take(0xB0,20,0));              // a release is no press
  assert(midi_switches_take(0xB0,20,127)); assert(ln==3&&lo==0&&saves==1);
  char t[32]; assert(midi_switches_describe(3,t,32)&&!strcmp(t,"CC 20 on channel 1"));
  assert(midi_switches_take(0xB0,20,0)); CHECK("");     // release of the learning press: nothing (not down)
  // press, short release
  assert(midi_switches_take(0xB0,20,127)); advance(100000); assert(midi_switches_take(0xB0,20,0)); CHECK("P3 R3 ");
  // other channel does not match
  assert(!midi_switches_take(0xB1,20,127));
  // hold
  assert(midi_switches_take(0xB0,20,127)); advance(599000); CHECK("P3 "); advance(2000); CHECK("H3 "); assert(midi_switches_take(0xB0,20,0)); CHECK("R3 ");
  // repeat 127 ignored
  midi_switches_take(0xB0,20,127); midi_switches_take(0xB0,20,127); midi_switches_take(0xB0,20,0); CHECK("P3 R3 ");
  // learn the same CC to 5 -> moves; note learn; PC learn
  midi_switches_learn(5); midi_switches_take(0xB0,20,127); assert(ln==5&&lo==3);
  assert(!midi_switches_describe(3,t,32)); midi_switches_take(0xB0,20,0); CHECK("");
  midi_switches_learn(1); midi_switches_take(0x92,60,100); assert(ln==1&&lo==0); midi_switches_take(0x82,60,0);
  midi_switches_take(0x92,60,90); midi_switches_take(0x92,60,0); CHECK("P1 R1 ");
  midi_switches_learn(2); midi_switches_take(0xC0,4,0); assert(ln==2); CHECK("");
  midi_switches_take(0xC0,4,0); CHECK("P2 R2 "); assert(!midi_switches_take(0xC0,5,0));
  // learn timeout
  ln=lo=-1; midi_switches_learn(4); advance(14000000); assert(ln==-1); advance(1100000); assert(ln==0&&lo==0);
  assert(!midi_switches_take(0xB0,30,127));            // no longer learning
  // cancel
  ln=-1; midi_switches_learn(4); midi_switches_learn(0); advance(16000000); assert(ln==-1); assert(!midi_switches_take(0xB0,30,127));
  // stored and loaded again
  uint16_t k[8]; memcpy(k,store,16); assert(k[4]==(0xB0<<8|20)&&k[0]==(0x92<<8|60)&&k[1]==(0xC0<<8|4));
  midi_switches_reset(); memcpy(k,store,16); for(int i=0;i<8;i++) assert(!k[i]); assert(!midi_switches_take(0xB0,20,127));
  puts("midi_switches: all checks passed"); return 0;
}
