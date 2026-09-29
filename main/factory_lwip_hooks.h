#pragma once

#ifdef __cplusplus
extern "C" {
#endif
struct pbuf;
struct netif;
int factory_timer_lwip_ip4_input_hook(struct pbuf *p, struct netif *inp);
#ifdef __cplusplus
}
#endif

#define LWIP_HOOK_IP4_INPUT(p, inp) factory_timer_lwip_ip4_input_hook((p), (inp))
