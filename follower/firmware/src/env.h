/*
 * env.h - persistent network "environment" for the bridge firmware.
 *
 * A versioned, CRC-protected network config (per-interface IP/mask/gateway/DNS)
 * stored in the Emulated EEPROM. Compiled defaults come from configuration.h; on
 * first boot (blank EEPROM) they are seeded. CLI: showenv/setenv/saveenv/readenv/resetenv.
 */
#ifndef ENV_H
#define ENV_H

/* Load the config from the Emulated EEPROM (or seed it from the compiled defaults
 * on first boot) and register the "env" CLI command group. Called once from
 * SYS_Initialize (initialization.c), right after EMU_EEPROM_Initialize() and
 * before TCPIP_STACK_Init() - so the persistent MAC is ready before the stack
 * binds it. */
void ENV_Init(void);

/* Push the current config into the TCP/IP stack (TCPIP_STACK_NetAddressSet/...) and
 * the LAN865x PLCA (via LAN865X_DIAG_ApplyPlca). Call once the stack is up
 * (APP_STATE_SERVICE_TASKS). */
void env_apply(void);

/* Current PLCA node id / node count from the env (eth0 / LAN865x). */
#include <stdint.h>
uint8_t env_plca_id(void);
uint8_t env_plca_cnt(void);

/* Format the env MAC for interface 0/1 as "XX:XX:XX:XX:XX:XX" into buf (>= 18 bytes).
 * Call after ENV_Init(), before TCPIP_STACK_Init(), to fill the stack's MAC strings. */
void env_mac_str(int iface, char *buf);

/* The MAC as bytes - the only identity a follower has before one is assigned,
   because it comes from the chip serial while the PLCA id and IP are compile
   defaults and therefore identical across a fleet. */
const uint8_t *env_mac(int iface);

/* Adopt an identity from the master.  ip_or_zero = 0 leaves the address alone.
   The value is in IPV4_ADDR.Val layout, NOT wire order - the caller owns the wire
   format and converts (see trig_cmd.c).  The PLCA id is applied to the PHY by the
   caller; the address is applied by env_apply_ip(). */
bool env_set_identity(uint8_t plca_id, uint8_t plca_cnt, uint32_t ip_or_zero,
                      bool persist);

/* Push the stored address of one interface into the live stack.
 *
 * Exists because "an assigned IP only takes effect after a reset" was wrong: this
 * is the same TCPIP_STACK_NetAddressSet() call env_apply() has always made at
 * start-up, and it works just as well later.  Believing otherwise is why the
 * master used to leave addresses alone and a whole fleet sat on one address -
 * including the bridge's own. */
bool env_apply_ip(int iface);

#endif /* ENV_H */
