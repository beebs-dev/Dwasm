/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  WASM/WebRTC network backend.
 *
 *  Implements the PrBoomX low-level UDP-ish API (I_* functions) by forwarding
 *  outgoing datagrams to JavaScript, and receiving incoming datagrams from
 *  JavaScript into a small queue.
 *
 *  The JS side is expected to transport datagrams over a lossy WebRTC data
 *  channel (LiveKit DataPacket_Kind.LOSSY), mimicking UDP.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_NET

#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "protocol.h"
#include "i_network.h"

// NOTE: <emscripten.h> (via em_js.h) defines true/false macros.
// Include it after Doom headers to avoid colliding with doomtype.h's dboolean.
#include <emscripten.h>

#ifndef PRBOOM_SERVER
extern void I_AtExit(void (*func)(void), dboolean run_if_error);
#endif

UDP_CHANNEL sentfrom;
IPaddress sentfrom_addr;
UDP_SOCKET udp_socket;

size_t sentbytes, recvdbytes;

#define WASM_NET_MAX_PACKET 10000
#define WASM_NET_QUEUE_LEN 64

typedef struct {
  int len;
  unsigned char data[WASM_NET_MAX_PACKET];
} wasm_net_pkt_t;

static wasm_net_pkt_t rxq[WASM_NET_QUEUE_LEN];
static int rxq_head;
static int rxq_tail;

static unsigned char *rx_scratch;
static int rx_scratch_cap;

static int rxq_is_empty(void)
{
  return rxq_head == rxq_tail;
}

static int rxq_is_full(void)
{
  return ((rxq_tail + 1) % WASM_NET_QUEUE_LEN) == rxq_head;
}

static void rxq_push_bytes(const unsigned char *data, int len)
{
  if (len <= 0)
    return;

  if (len > WASM_NET_MAX_PACKET)
    len = WASM_NET_MAX_PACKET;

  if (rxq_is_full()) {
    // Drop when full (UDP semantics).
    return;
  }

  memcpy(rxq[rxq_tail].data, data, (size_t)len);
  rxq[rxq_tail].len = len;
  rxq_tail = (rxq_tail + 1) % WASM_NET_QUEUE_LEN;
}

static int rxq_pop(unsigned char *out, int outcap)
{
  int len;

  if (rxq_is_empty())
    return 0;

  len = rxq[rxq_head].len;
  if (len > outcap)
    len = outcap;

  memcpy(out, rxq[rxq_head].data, (size_t)len);
  rxq_head = (rxq_head + 1) % WASM_NET_QUEUE_LEN;

  return len;
}

static byte ChecksumPacket(const packet_header_t *buffer, size_t len)
{
  const byte *p = (const byte *)buffer;
  byte sum = 0;

  if (len == 0)
    return 0;

  while (p++, --len)
    sum += *p;

  return sum;
}

EM_JS(void, wasm_net_js_connect, (const char *serv), {
  if (!Module.dwasmNet || typeof Module.dwasmNet.connect !== 'function') {
    console.warn('dwasmNet bridge not present; cannot connect');
    return;
  }
  const s = UTF8ToString(serv);
  Module.dwasmNet.connect(s);
});

EM_JS(void, wasm_net_js_disconnect, (), {
  if (Module.dwasmNet && typeof Module.dwasmNet.disconnect === 'function') {
    Module.dwasmNet.disconnect();
  }
});

EM_JS(void, wasm_net_js_send, (const unsigned char *data, int len), {
  if (!Module.dwasmNet || typeof Module.dwasmNet.send !== 'function') {
    // Silently drop.
    return;
  }
  const bytes = HEAPU8.subarray(data, data + len);
  // Copy into a standalone Uint8Array for transport.
  Module.dwasmNet.send(new Uint8Array(bytes));
});

// JS calls this two-step API to avoid needing Module._malloc/_free.
EMSCRIPTEN_KEEPALIVE unsigned char *wasm_net_get_rx_buf(int len)
{
  if (len <= 0)
    len = 0;

  if (len > WASM_NET_MAX_PACKET)
    len = WASM_NET_MAX_PACKET;

  if (len > rx_scratch_cap) {
    unsigned char *p = (unsigned char *)realloc(rx_scratch, (size_t)len);
    if (!p)
      return 0;
    rx_scratch = p;
    rx_scratch_cap = len;
  }

  return rx_scratch;
}

EMSCRIPTEN_KEEPALIVE void wasm_net_commit_rx(int len)
{
  if (!rx_scratch || len <= 0)
    return;

  if (len > rx_scratch_cap)
    len = rx_scratch_cap;

  rxq_push_bytes(rx_scratch, len);
}

void I_ShutdownNetwork(void)
{
  wasm_net_js_disconnect();
  free(rx_scratch);
  rx_scratch = 0;
  rx_scratch_cap = 0;
}

void I_InitNetwork(void)
{
  rxq_head = rxq_tail = 0;
#ifndef PRBOOM_SERVER
  // Best-effort cleanup when exiting.
  // (Browser builds usually won't truly exit, but keep parity.)
  // NOTE: I_AtExit is only available in non-server builds.
  I_AtExit(I_ShutdownNetwork, 1);
#else
  atexit(I_ShutdownNetwork);
#endif
}

void I_WaitForPacket(int ms)
{
  // In the browser we can't select() on sockets; just yield.
  if (ms > 0)
    SDL_Delay((Uint32)ms);
}

UDP_SOCKET I_Socket(Uint16 port)
{
  (void)port;
  udp_socket = 1;
  return udp_socket;
}

void I_CloseSocket(UDP_SOCKET sock)
{
  (void)sock;
  udp_socket = 0;
}

int I_ConnectToServer(const char *serv)
{
  if (!serv || !*serv)
    return -1;

  wasm_net_js_connect(serv);
  return 0;
}

void I_Disconnect(void)
{
  wasm_net_js_disconnect();
}

size_t I_GetPacket(packet_header_t *buffer, size_t buflen)
{
  int len;
  int checksum;

  if (!buffer || buflen == 0)
    return 0;

  len = rxq_pop((unsigned char *)buffer, (int)buflen);
  if (len <= 0)
    return 0;

  sentfrom = 0;
  sentfrom_addr.host = 0;
  sentfrom_addr.port = 0;

  checksum = buffer->checksum;
  buffer->checksum = 0;

  // Verify packet checksum like the SDL_net backend.
  if (ChecksumPacket(buffer, (size_t)len) != (byte)checksum)
    return 0;

  recvdbytes += (size_t)len;
  return (size_t)len;
}

void I_SendPacket(packet_header_t *packet, size_t len)
{
  if (!packet || len == 0)
    return;

  if (len > WASM_NET_MAX_PACKET)
    len = WASM_NET_MAX_PACKET;

  packet->checksum = ChecksumPacket(packet, len);
  sentbytes += len;
  wasm_net_js_send((const unsigned char *)packet, (int)len);
}

void I_SendPacketTo(packet_header_t *packet, size_t len, UDP_CHANNEL *to)
{
  (void)to;
  I_SendPacket(packet, len);
}

UDP_PACKET *I_AllocPacket(int size)
{
  UDP_PACKET *p;

  if (size <= 0)
    size = 1;

  p = (UDP_PACKET *)calloc(1, sizeof(*p));
  if (!p)
    return 0;

  p->data = (Uint8 *)malloc((size_t)size);
  if (!p->data) {
    free(p);
    return 0;
  }

  p->maxlen = size;
  p->len = 0;
  p->channel = 0;
  p->address.host = 0;
  p->address.port = 0;

  return p;
}

void I_FreePacket(UDP_PACKET *packet)
{
  if (!packet)
    return;
  free(packet->data);
  free(packet);
}

void I_UnRegisterPlayer(UDP_CHANNEL channel)
{
  (void)channel;
}

UDP_CHANNEL I_RegisterPlayer(IPaddress *ipaddr)
{
  (void)ipaddr;
  return 0;
}

void I_PrintAddress(FILE *fp, UDP_CHANNEL *addr)
{
  (void)fp;
  (void)addr;
}

#endif /* HAVE_NET */
