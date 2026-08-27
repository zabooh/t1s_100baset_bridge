/*******************************************************************************
  Endpoint server - implementation

  See someip.h for the wire format and the origin of every number.

  Layout of this file, readable bottom to top:
      1. WTLV encoder and decoder
      2. build and check the header
      3. handle management
      4. the methods - each a THIN ADAPTER: parse, call ONE existing
         function, encode the reply
      5. dispatch table
      6. socket and tasks

  THE HANDLERS ARE ADAPTERS, NOT LOGIC.  Everything that has an effect
  already exists: `PIN_Set/Get/Claim/Release` and
  `PTP_TRIG_SchedulePeriodic/ScheduleAt/Cancel/PulseSet`.  The moment a
  handler does more than translate, the effect migrates into the protocol
  file and can no longer be tested from there without it.
*******************************************************************************/

#include <string.h>
#include "someip.h"
#include "pin_table.h"
#include "ptp_trigger.h"
#include "ptp_timebase.h"
#include "definitions.h"

#define EPSRV_IF            0            /* eth0, the T1S port                  */
#define EPSRV_BUF_MAX       512u         /* PORTING.md names 512 as the no-OTA value */
#define EPSRV_PWM_ACTION    1u           /* the trigger's action slot for PWM   */

static UDP_SOCKET   s_sock = INVALID_UDP_SOCKET;
static uint8_t      s_rx[EPSRV_BUF_MAX];
static uint8_t      s_tx[EPSRV_BUF_MAX];
static EPSRV_HANDLE s_hnd[EPSRV_HANDLES_MAX];
static uint16_t     s_next_handle = 1u;   /* 0 is invalid                       */
static bool         s_log;
static EPSRV_STATUS s_st;
static uint64_t     s_up_ticks;
static uint32_t     s_ticks_per_ms;

/* Why a counter of its own for the last reason: `RT_NOT_READY` has three
 * causes (pin taken, no channel free, time base not usable), and all three
 * look identical on the wire - that is their given.  On the console they
 * must be distinguishable, or S3.6 cannot be signed off. */
static const char *s_last_why = "-";

/* --------------------------------------------------------------------------- */
/* 1. WTLV                                                                     */
/* --------------------------------------------------------------------------- */

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

/* Tag header.  Two bytes, and the upper half of the first carries the
   wiretype - so the tag id is 12 bit, not 16 (someip-gen.c:83). */
static bool wt_tag(uint8_t *buf, uint16_t cap, uint16_t *used,
                   uint16_t tag, uint8_t wt)
{
    if (tag > 0x0FFFu || (uint16_t)(*used + 2u) > cap) { return false; }
    buf[*used]      = (uint8_t)((wt << 4) | ((tag >> 8) & 0x0Fu));
    buf[*used + 1u] = (uint8_t)(tag & 0xFFu);
    *used = (uint16_t)(*used + 2u);
    return true;
}

static bool wt_u8(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag, uint8_t v)
{
    if (!wt_tag(buf, cap, used, tag, EPSRV_WT_U8)) { return false; }
    if ((uint16_t)(*used + 1u) > cap) { return false; }
    buf[(*used)++] = v;
    return true;
}

static bool wt_u16(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag, uint16_t v)
{
    if (!wt_tag(buf, cap, used, tag, EPSRV_WT_U16)) { return false; }
    if ((uint16_t)(*used + 2u) > cap) { return false; }
    put_be16(&buf[*used], v); *used = (uint16_t)(*used + 2u);
    return true;
}

static bool wt_u32(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag, uint32_t v)
{
    if (!wt_tag(buf, cap, used, tag, EPSRV_WT_U32)) { return false; }
    if ((uint16_t)(*used + 4u) > cap) { return false; }
    put_be32(&buf[*used], v); *used = (uint16_t)(*used + 4u);
    return true;
}

static bool wt_u64(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag, uint64_t v)
{
    if (!wt_tag(buf, cap, used, tag, EPSRV_WT_U64)) { return false; }
    if ((uint16_t)(*used + 8u) > cap) { return false; }
    put_be32(&buf[*used], (uint32_t)(v >> 32));
    put_be32(&buf[*used + 4u], (uint32_t)v);
    *used = (uint16_t)(*used + 8u);
    return true;
}

static bool wt_blob(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag,
                    const uint8_t *data, uint16_t len)
{
    /* ALWAYS wiretype 6 with a length field, even at length 0.
     *
     * The first build here followed THEIR GENERATOR: `Fill_Tag` uses
     * wiretype 4 (static size, no length field) at length 0.  But THEIR
     * PARSER does not know that - `SOMEIP_Parser_Read_BLOB` accepts only
     * `WIRETYPE_COMPLEX_2_BYTE` and asserts otherwise.  MEASURED
     * 2026-08-16: `lan866x-discovery` found both followers and then aborted
     * in `someip-pars.c:244`, `GetStatus failed (rc=9)`.
     *
     * MNEMONIC: a generator and a parser from the same library are not
     * necessarily symmetric.  Whoever builds to the generator produces
     * frames their own parser rejects - and the error message points at the
     * payload, not at the asymmetry. */
    if (!wt_tag(buf, cap, used, tag, EPSRV_WT_BLOB)) { return false; }
    if ((uint16_t)(*used + 2u + len) > cap) { return false; }
    put_be16(&buf[*used], len); *used = (uint16_t)(*used + 2u);
    memcpy(&buf[*used], data, len); *used = (uint16_t)(*used + len);
    return true;
}

static bool wt_str(uint8_t *buf, uint16_t cap, uint16_t *used, uint16_t tag,
                   const char *s)
{
    return wt_blob(buf, cap, used, tag, (const uint8_t *)s, (uint16_t)strlen(s));
}

/* Read with a tag expectation.  If the tag does not match, the field counts
   as OMITTED, nothing is consumed, and `false` is NOT reported - exactly
   like `Read_*_ExpectTag` (someip-pars.c:315).  `*found` says whether
   something was actually read; whoever expects a mandatory value checks
   that. */
static bool wt_rd(const uint8_t *buf, uint16_t len, uint16_t *pos, uint16_t tag,
                  uint8_t wt, uint64_t *out, bool *found)
{
    uint16_t need = (uint16_t)(1u << wt);      /* 1, 2, 4, 8 for wt 0..3 */
    *found = false;
    if ((uint16_t)(*pos + 2u) > len) { return true; }
    if ((((uint16_t)(buf[*pos] & 0x0Fu) << 8) | buf[*pos + 1u]) != tag) { return true; }
    if (((buf[*pos] >> 4) & 0x07u) != wt) { return false; }
    if ((uint16_t)(*pos + 2u + need) > len) { return false; }
    switch (wt)
    {
        case EPSRV_WT_U8:  *out = buf[*pos + 2u]; break;
        case EPSRV_WT_U16: *out = rd_be16(&buf[*pos + 2u]); break;
        case EPSRV_WT_U32: *out = rd_be32(&buf[*pos + 2u]); break;
        default:           *out = rd_be64(&buf[*pos + 2u]); break;
    }
    *pos = (uint16_t)(*pos + 2u + need);
    *found = true;
    return true;
}

static bool wt_rd_blob(const uint8_t *buf, uint16_t len, uint16_t *pos, uint16_t tag,
                       const uint8_t **data, uint16_t *dlen, bool *found)
{
    *found = false; *dlen = 0u; *data = NULL;
    if ((uint16_t)(*pos + 2u) > len) { return true; }
    if ((((uint16_t)(buf[*pos] & 0x0Fu) << 8) | buf[*pos + 1u]) != tag) { return true; }
    {
        uint8_t wt = (uint8_t)((buf[*pos] >> 4) & 0x07u);
        if (wt == 0x4u)                       /* empty blob, no length field */
        {
            *pos = (uint16_t)(*pos + 2u); *found = true; return true;
        }
        if (wt != EPSRV_WT_BLOB) { return false; }
    }
    if ((uint16_t)(*pos + 4u) > len) { return false; }
    *dlen = rd_be16(&buf[*pos + 2u]);
    if ((uint16_t)(*pos + 4u + *dlen) > len) { return false; }
    *data = &buf[*pos + 4u];
    *pos  = (uint16_t)(*pos + 4u + *dlen);
    *found = true;
    return true;
}

/* --------------------------------------------------------------------------- */
/* 2. Kopf                                                                     */
/* --------------------------------------------------------------------------- */

typedef struct
{
    uint16_t service, method, client, session;
    uint32_t length;
    uint8_t  proto, iface, msgtype, retcode;
} ep_hdr_t;

static bool hdr_parse(const uint8_t *b, uint16_t len, ep_hdr_t *h)
{
    if (len < EPSRV_HDR_LEN) { return false; }
    h->service = rd_be16(b);
    h->method  = rd_be16(b + 2);
    h->length  = rd_be32(b + 4);
    h->client  = rd_be16(b + 8);
    h->session = rd_be16(b + 10);
    h->proto   = b[12];
    h->iface   = b[13];
    h->msgtype = b[14];
    h->retcode = b[15];
    /* The length counts from byte 8, NOT the whole message (someip-gen.c:66).
       That is this format's documented trap, hence checked rather than
       trusted. */
    if (h->length < 8u) { return false; }
    if ((uint32_t)(h->length - 8u) > (uint32_t)(len - EPSRV_HDR_LEN)) { return false; }
    return true;
}

static uint16_t hdr_build(uint8_t *b, const ep_hdr_t *in, uint8_t msgtype, uint8_t rt)
{
    put_be16(b, EPSRV_SERVICE_ID);
    put_be16(b + 2, in->method);
    put_be32(b + 4, 8u);                  /* filled in at the end               */
    /* MIRROR BACK CLIENT ID AND SESSION ID.  The host matches the reply via
       `s_waitSid`; without mirroring it waits into a timeout, which looks
       like a dead device instead of a protocol error. */
    put_be16(b + 8, in->client);
    put_be16(b + 10, in->session);
    b[12] = EPSRV_PROTO_VER;
    b[13] = EPSRV_IFACE_VER;
    b[14] = msgtype;
    b[15] = rt;
    return EPSRV_HDR_LEN;
}

/* --------------------------------------------------------------------------- */
/* 3. Handles                                                                  */
/* --------------------------------------------------------------------------- */

static EPSRV_HANDLE *hnd_find(uint16_t id)
{
    for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++)
    {
        if (s_hnd[i].open && s_hnd[i].id == id) { return &s_hnd[i]; }
    }
    return NULL;
}

static EPSRV_HANDLE *hnd_by_pin(uint8_t pin_index)
{
    for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++)
    {
        if (s_hnd[i].open && s_hnd[i].pin_index == pin_index) { return &s_hnd[i]; }
    }
    return NULL;
}

static EPSRV_HANDLE *hnd_alloc(void)
{
    for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++)
    {
        if (!s_hnd[i].open)
        {
            memset(&s_hnd[i], 0, sizeof s_hnd[i]);
            s_hnd[i].open = true;
            s_hnd[i].id   = s_next_handle++;
            if (s_next_handle == 0u) { s_next_handle = 1u; }
            s_st.handles_open++;
            return &s_hnd[i];
        }
    }
    return NULL;
}

static void hnd_free(EPSRV_HANDLE *h)
{
    if (h == NULL || !h->open) { return; }
    /* A PWM handle holds the trigger.  Releasing therefore means: stop the
       signal first, then return the pin - the other way round a pin keeps
       toggling that nobody owns any more, and that was exactly E24. */
    if (h->kind == 1u)
    {
        PTP_TRIG_Cancel();
        PTP_TRIG_PulseSet(false, 0u);
    }
    PIN_Release(h->pin_index, (h->kind == 1u) ? PIN_OWNER_PWM : PIN_OWNER_GPIO);
    h->open = false;
    if (s_st.handles_open > 0u) { s_st.handles_open--; }
}

/* --------------------------------------------------------------------------- */
/* 4. Methods                                                                  */
/* --------------------------------------------------------------------------- */

/* A handler receives the payload and writes its reply payload; it returns
   the return code.  `*used` starts at 0 and counts ONLY the payload - the
   header is already in place. */
typedef uint8_t (*ep_handler)(const uint8_t *pl, uint16_t pl_len,
                             uint8_t *out, uint16_t cap, uint16_t *used);

/* The grandmaster time, or 0.  Here NULL means "I don't have it", and that
   is a statement - not an error: the host reads `WallclockState` alongside
   it, so it knows whether the zero means ignorance or midnight. */
static uint64_t master_now_ns(void)
{
    uint64_t ns = 0u;
    return PTP_TB_Now(&ns) ? ns : 0u;
}

static uint8_t m_get_status(const uint8_t *pl, uint16_t pl_len,
                            uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t up_ns;
    (void)pl; (void)pl_len;

    /* The tag sequence is the host's (lan866x_client.c GetStatus) and must
       arrive complete and IN THIS ORDER - the parser runs through it
       sequentially.  A missing tag in the middle makes everything after it
       count as "omitted", so the reply then goes silently incomplete. */
    up_ns = (s_ticks_per_ms != 0u)
          ? ((SYS_TIME_Counter64Get() - s_up_ticks) / s_ticks_per_ms) * 1000000ULL
          : 0u;

    return (wt_str(o, cap, u, 0u, "t1s-follower")
         /* THE REAL CHIP, not LAN8661A.  `discovery.c` maps LAN8660/1/2 to
            "Control/Lighting/Audio Endpoint" and EVERYTHING ELSE to the
            neutral "Endpoint" - the fallback is intended.  A made-up name
            costs nothing in compatibility and ruins every later diagnosis
            that leans on this string. */
         && wt_str(o, cap, u, 1u, "ATSAME54P20A")
         /* NO EMPTY BLOBS.  Their parser checks a blob's UTF8 BOM at
            pBuf[4..6] - at length 0 it therefore reads past the end into the
            next tag, and if it happens to land on EF BB BF there, it
            computes `blobLen - 3` on a uint16.  A short placeholder costs
            three bytes and keeps its gaze inside our data. */
         && wt_str(o, cap, u, 2u, "-")           /* RootApplicationVersion       */
         && wt_str(o, cap, u, 3u, "-")           /* BootApplicationVersion       */
         && wt_str(o, cap, u, 4u, "-")           /* BootConfigurationVersion     */
         && wt_str(o, cap, u, 5u, "t1s-1.0")     /* MainApplicationVersion       */
         && wt_str(o, cap, u, 6u, "-")           /* MainConfigurationVersion     */
         && wt_u64(o, cap, u, 7u, 0u)            /* StartupInformation           */
         && wt_u64(o, cap, u, 8u, up_ns)         /* UpTime                       */
         /* THE LOWEST VERSION that covers what we can do.  A reported
            version is a PROMISE about the feature set - the host derives
            from it what it is allowed to try.  Reported too high, it calls
            methods that do not exist here and takes our correct
            RT_UNKNOWN_METHOD for a defect.  Format per lan866x_common.h:323
            "0x<%02X:major><%02X:minor>00", i.e. major in the top byte. */
         && wt_u32(o, cap, u, 9u, 0x01000000u)   /* ComoVersion    V1.0         */
         && wt_u32(o, cap, u, 10u, 0x01000000u)  /* ServiceVersion V1.0         */
         && wt_str(o, cap, u, 11u, "-")          /* KeysVersion                  */
         && wt_u64(o, cap, u, 12u, 0u)           /* DeviceId                     */
         && wt_u8(o, cap, u, 13u, 0u)            /* ResetCounter                 */
         && wt_str(o, cap, u, 14u, "-")          /* LastExceptionData0           */
         && wt_str(o, cap, u, 15u, "-")          /* LastExceptionData1           */
         && wt_u32(o, cap, u, 16u, 0u)           /* SingleBitErrorCounts         */
         && wt_u32(o, cap, u, 17u, 0u)           /* PowerStatus                  */
         /* The wallclock state is NOT made up: it comes from the time
            layer, and it is the most interesting number this follower can
            report to a foreign host at all. */
         && wt_u8(o, cap, u, 18u, (uint8_t)PTP_TB_QualityGet())
         && wt_u64(o, cap, u, 19u, master_now_ns()))
        ? EPSRV_RT_OK : EPSRV_RT_NOT_READY;
}

static uint8_t m_open_gpio(const uint8_t *pl, uint16_t pl_len,
                           uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f;
    uint16_t p = 0u;
    uint8_t pin, dir;
    const PIN_ROW *row;
    EPSRV_HANDLE *h;

    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U8, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    pin = (uint8_t)v;
    if (!wt_rd(pl, pl_len, &p, 1u, EPSRV_WT_U8, &v, &f)) { return EPSRV_RT_NOT_REACHABLE; }
    dir = f ? (uint8_t)v : 0u;

    /* An index above 15 is NOT ACCEPTED AT ALL - the index space is 0..15,
       because `PinId` is a uint8_t with 0xFF as "unused". */
    row = (pin < PIN_INDEX_MAX) ? PIN_Find(pin) : NULL;
    if (row == NULL) { s_last_why = "pin index not mapped"; return EPSRV_RT_NOT_REACHABLE; }

    h = hnd_by_pin(pin);
    if (h != NULL)
    {
        /* Already open: return the same handle instead of creating a second
           one.  Two handles on one pin would be two owners with one name. */
        return wt_u16(o, cap, u, 0u, h->id) ? EPSRV_RT_OK : EPSRV_RT_NOT_READY;
    }
    if (!PIN_Claim(pin, PIN_OWNER_GPIO))
    {
        s_last_why = "pin held by another owner";
        return EPSRV_RT_NOT_READY;
    }
    h = hnd_alloc();
    if (h == NULL)
    {
        PIN_Release(pin, PIN_OWNER_GPIO);
        s_last_why = "no handle slot free";
        return EPSRV_RT_NOT_READY;
    }
    h->pin_index = pin; h->kind = 0u; h->direction = dir;
    /* 0 input, 1 output-low, 2 output-high, 3 open-drain.  Input is not
       reconfigured here: the table rows are outputs, and a silent direction
       change on a pin the trigger drives would be worse than an honest "no
       can do". */
    if (dir == 1u) { PIN_Set(row, false); }
    if (dir == 2u) { PIN_Set(row, true); }
    return wt_u16(o, cap, u, 0u, h->id) ? EPSRV_RT_OK : EPSRV_RT_NOT_READY;
}

/* SetGpio carries up to 16 tuples {u16 handle, u8 value} in ONE blob - which
   is why a host can set three LEDs with one frame, and why `ledblink` is
   simultaneous at all. */
static uint8_t gpio_set_from_blob(const uint8_t *d, uint16_t dl)
{
    uint8_t rt = EPSRV_RT_OK;
    for (uint16_t i = 0u; (uint16_t)(i + 3u) <= dl; i = (uint16_t)(i + 3u))
    {
        EPSRV_HANDLE *h = hnd_find(rd_be16(&d[i]));
        const PIN_ROW *row = (h != NULL) ? PIN_Find(h->pin_index) : NULL;
        if (row == NULL) { rt = EPSRV_RT_NOT_REACHABLE; continue; }
        PIN_Set(row, d[i + 2u] != 0u);
    }
    return rt;
}

static uint8_t m_set_gpio(const uint8_t *pl, uint16_t pl_len,
                          uint8_t *o, uint16_t cap, uint16_t *u)
{
    const uint8_t *d; uint16_t dl; bool f; uint16_t p = 0u;
    (void)o; (void)cap; (void)u;
    if (!wt_rd_blob(pl, pl_len, &p, 0u, &d, &dl, &f) || !f)
    {
        return EPSRV_RT_NOT_REACHABLE;
    }
    return gpio_set_from_blob(d, dl);
}

static uint8_t m_set_gpio_faf(const uint8_t *pl, uint16_t pl_len,
                              uint8_t *o, uint16_t cap, uint16_t *u)
{
    return m_set_gpio(pl, pl_len, o, cap, u);   /* Wirkung gleich, Antwort entfaellt */
}

static uint8_t m_get_gpio(const uint8_t *pl, uint16_t pl_len,
                          uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint8_t tup[EPSRV_HANDLES_MAX * 3u];
    uint16_t n = 0u;
    (void)pl; (void)pl_len;

    for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++)
    {
        const PIN_ROW *row;
        if (!s_hnd[i].open || s_hnd[i].kind != 0u) { continue; }
        row = PIN_Find(s_hnd[i].pin_index);
        if (row == NULL) { continue; }
        put_be16(&tup[n], s_hnd[i].id); n = (uint16_t)(n + 2u);
        tup[n++] = PIN_Get(row) ? 1u : 0u;
    }
    return wt_blob(o, cap, u, 0u, tup, n) ? EPSRV_RT_OK : EPSRV_RT_NOT_READY;
}

static uint8_t m_release_pins(const uint8_t *pl, uint16_t pl_len,
                              uint8_t *o, uint16_t cap, uint16_t *u)
{
    const uint8_t *d; uint16_t dl; bool f; uint16_t p = 0u;
    (void)o; (void)cap; (void)u;

    if (!wt_rd_blob(pl, pl_len, &p, 0u, &d, &dl, &f)) { return EPSRV_RT_NOT_REACHABLE; }
    /* No blob or empty means "release everything".  That is the order
       EVERY one of their GPIO tools runs: ReleaseDigitalPins first, then
       OpenGpio - so a tool cleans up whatever a previous run left open. */
    if (!f || dl == 0u)
    {
        for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++) { hnd_free(&s_hnd[i]); }
        return EPSRV_RT_OK;
    }
    for (uint16_t i = 0u; i < dl; i++)
    {
        if (d[i] == PIN_INDEX_UNUSED) { continue; }   /* 0xFF = unused slot */
        hnd_free(hnd_by_pin(d[i]));
    }
    return EPSRV_RT_OK;
}

/* Q31 -> percent.  DutyCycle 0 = 0 %, 2^31 = 100 % (their sample PWM file
   explicitly names 0x80000000 as 100 %).  The trigger can do 1..99 %, so
   it is clamped and the clamp is counted rather than hidden. */
static uint32_t duty_q31_to_pct(uint32_t q31)
{
    uint64_t pct = ((uint64_t)q31 * 100ULL) / 2147483648ULL;
    if (pct < 1u)  { pct = 1u; }
    if (pct > 99u) { pct = 99u; }
    return (uint32_t)pct;
}

static uint8_t pwm_apply(EPSRV_HANDLE *h, uint16_t session)
{
    PTP_TRIG_RESULT r;
    /* Their protocol OWES the phase - `OpenPwm` has no start field.  So we
     * fill it in ourselves, and in a way that gives two boards the same
     * one: first rising edge wherever `t mod IntervalTime == 0` holds on
     * the SHARED time base.  That is phase_ns = 0, start_ns = 0.
     *
     * That is exactly the gain an endpoint with its own PWM counter does
     * not have: TCC0 runs on the local crystal and has no relation to
     * grandmaster time. */
    PTP_TRIG_PulseSet(true, duty_q31_to_pct(h->duty_q31));
    /* count = 0: a PWM runs unlimited until switched off. */
    r = PTP_TRIG_SchedulePeriodic(EPSRV_PWM_ACTION, session,
                                  (uint64_t)h->interval_ns, 0u, 0u, 0u);
    if (r == PTP_TRIG_OK) { return EPSRV_RT_OK; }
    /* All three rejection reasons are RT_NOT_READY on the wire -
       distinguishable only here. */
    s_last_why = PTP_TRIG_ResultName(r);
    return (r == PTP_TRIG_ERR_NO_TIME) ? EPSRV_RT_NOT_READY : EPSRV_RT_NOT_READY;
}

static uint8_t m_open_pwm(const uint8_t *pl, uint16_t pl_len,
                          uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f; uint16_t p = 0u;
    uint8_t pin;
    uint32_t interval, duty;
    const PIN_ROW *row;
    EPSRV_HANDLE *h;
    uint8_t rt;

    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U8, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    pin = (uint8_t)v;
    if (!wt_rd(pl, pl_len, &p, 1u, EPSRV_WT_U32, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    interval = (uint32_t)v;
    if (!wt_rd(pl, pl_len, &p, 2u, EPSRV_WT_U32, &v, &f)) { return EPSRV_RT_NOT_REACHABLE; }
    duty = f ? (uint32_t)v : 0x40000000u;      /* no value given: 50 %           */

    /* THE THREE FAILURE CASES, and they are deliberately distinguishable (S3.6):
       1. no PWM path on this pin     -> 0x05, "no pwm path"
       2. pin already owned           -> 0x04, "pin held by another owner"
       3. no handle slot free         -> 0x04, "no handle slot free" */
    row = (pin < PIN_INDEX_MAX) ? PIN_Find(pin) : NULL;
    if (row == NULL) { s_last_why = "pin index not mapped"; return EPSRV_RT_NOT_REACHABLE; }
    if (!row->can_pwm) { s_last_why = "no pwm path on this pin"; return EPSRV_RT_NOT_REACHABLE; }
    if (interval < (uint32_t)PTP_TRIG_MIN_PERIOD_US * 1000u)
    {
        s_last_why = "interval below the measured period floor";
        return EPSRV_RT_NOT_READY;
    }

    h = hnd_by_pin(pin);
    if (h != NULL && h->kind != 1u)
    {
        s_last_why = "pin held by a gpio handle";
        return EPSRV_RT_NOT_READY;
    }
    if (h == NULL)
    {
        if (!PIN_Claim(pin, PIN_OWNER_PWM))
        {
            s_last_why = "pin held by another owner";
            return EPSRV_RT_NOT_READY;
        }
        h = hnd_alloc();
        if (h == NULL)
        {
            PIN_Release(pin, PIN_OWNER_PWM);
            s_last_why = "no handle slot free";
            return EPSRV_RT_NOT_READY;
        }
        h->pin_index = pin; h->kind = 1u;
    }
    h->interval_ns = interval; h->duty_q31 = duty;
    rt = pwm_apply(h, h->id);
    if (rt != EPSRV_RT_OK) { hnd_free(h); return rt; }
    return wt_u16(o, cap, u, 0u, h->id) ? EPSRV_RT_OK : EPSRV_RT_NOT_READY;
}

static uint8_t m_write_pwm(const uint8_t *pl, uint16_t pl_len,
                           uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f; uint16_t p = 0u;
    EPSRV_HANDLE *h;
    (void)o; (void)cap; (void)u;

    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U16, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    h = hnd_find((uint16_t)v);
    if (h == NULL || h->kind != 1u) { s_last_why = "no such pwm handle"; return EPSRV_RT_NOT_REACHABLE; }
    (void)wt_rd(pl, pl_len, &p, 1u, EPSRV_WT_U32, &v, &f);      /* WriteId, ungenutzt */
    if (!wt_rd(pl, pl_len, &p, 2u, EPSRV_WT_U32, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    h->duty_q31 = (uint32_t)v;
    /* ONLY the duty cycle, no re-arming: restarting the grid would shift
       the phase, and the phase is the whole point here. */
    PTP_TRIG_PulseSet(true, duty_q31_to_pct(h->duty_q31));
    return EPSRV_RT_OK;
}

static uint8_t m_close_pwm(const uint8_t *pl, uint16_t pl_len,
                           uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f; uint16_t p = 0u;
    (void)o; (void)cap; (void)u;
    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U16, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    {
        EPSRV_HANDLE *h = hnd_find((uint16_t)v);
        if (h == NULL) { return EPSRV_RT_NOT_REACHABLE; }
        hnd_free(h);
    }
    return EPSRV_RT_OK;
}

/* `EnableGpioPulseEvent 0x1350` is the most interesting entry in their whole
 * protocol: it describes exactly our trigger, just in their words.
 * StartTimeRelative 0 means ABSOLUTE time - and because our time base is the
 * grandmaster's, that makes every board of a fleet fire on the same instant,
 * with the host never needing to know. */
static uint8_t m_enable_pulse(const uint8_t *pl, uint16_t pl_len,
                              uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f; uint16_t p = 0u;
    EPSRV_HANDLE *h;
    uint8_t rel;
    uint64_t start_ns = 0u;
    uint32_t pulse_ns = 0u, idle_ns = 0u;
    PTP_TRIG_RESULT r;
    (void)o; (void)cap; (void)u;

    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U16, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    h = hnd_find((uint16_t)v);
    if (h == NULL) { s_last_why = "no such gpio handle"; return EPSRV_RT_NOT_REACHABLE; }
    (void)wt_rd(pl, pl_len, &p, 1u, EPSRV_WT_U8, &v, &f);        /* Notification  */
    (void)wt_rd(pl, pl_len, &p, 2u, EPSRV_WT_U8, &v, &f);        /* StartEdge     */
    (void)wt_rd(pl, pl_len, &p, 3u, EPSRV_WT_U8, &v, &f);
    rel = f ? (uint8_t)v : 0u;
    (void)wt_rd(pl, pl_len, &p, 4u, EPSRV_WT_U64, &v, &f);
    start_ns = f ? v : 0u;
    if (!wt_rd(pl, pl_len, &p, 5u, EPSRV_WT_U32, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    pulse_ns = (uint32_t)v;
    (void)wt_rd(pl, pl_len, &p, 6u, EPSRV_WT_U32, &v, &f);
    idle_ns = f ? (uint32_t)v : 0u;
    /* Count exists only in SDK header v1.10.0, not in the vendored one
       `rcp.c` is built against - so read it optionally and ignore it.  With
       two copies of one vendor struct, the one built against is the one
       that counts. */
    (void)wt_rd(pl, pl_len, &p, 7u, EPSRV_WT_U32, &v, &f);

    /* PulseTime must not be 0 - their own requirement, and it makes sense:
       a pulse with no high time is no pulse. */
    if (pulse_ns == 0u) { s_last_why = "PulseTime must not be 0"; return EPSRV_RT_NOT_READY; }
    /* A relative start time is REJECTED rather than approximated: it would
       let every node compute its own, and that is exactly where
       simultaneity measurably failed (3 of 30 armings a whole period off). */
    if (rel != 0u) { s_last_why = "StartTimeRelative=1 not supported"; return EPSRV_RT_NOT_READY; }

    if (!PIN_Claim(h->pin_index, PIN_OWNER_PWM))
    {
        s_last_why = "pin held by another owner";
        return EPSRV_RT_NOT_READY;
    }
    h->kind = 1u;
    if (idle_ns == 0u)
    {
        /* IdleTime 0 = SINGLE PULSE.  A one-shot needs an instant, not a
           period - and without StartTime there is none. */
        if (start_ns == 0u) { s_last_why = "single pulse needs StartTime"; return EPSRV_RT_NOT_READY; }
        PTP_TRIG_PulseSet(true, 50u);
        r = PTP_TRIG_ScheduleAt(EPSRV_PWM_ACTION, h->id, start_ns);
    }
    else
    {
        uint64_t per = (uint64_t)pulse_ns + (uint64_t)idle_ns;
        uint32_t pct = (uint32_t)(((uint64_t)pulse_ns * 100ULL) / per);
        if (pct < 1u)  { pct = 1u; }
        if (pct > 99u) { pct = 99u; }
        h->interval_ns = (uint32_t)per;
        h->duty_q31 = (uint32_t)(((uint64_t)pulse_ns * 2147483648ULL) / per);
        PTP_TRIG_PulseSet(true, pct);
        r = PTP_TRIG_SchedulePeriodic(EPSRV_PWM_ACTION, h->id, per, 0u,
                                      start_ns, 0u);   /* unbegrenzt */
    }
    if (r != PTP_TRIG_OK)
    {
        s_last_why = PTP_TRIG_ResultName(r);
        return EPSRV_RT_NOT_READY;
    }
    return EPSRV_RT_OK;
}

static uint8_t m_enable_capture(const uint8_t *pl, uint16_t pl_len,
                                uint8_t *o, uint16_t cap, uint16_t *u)
{
    (void)pl; (void)pl_len; (void)o; (void)cap; (void)u;
    /* AN HONEST NO.  Edge capture needs an input with an EIC line; the three
       table rows are outputs, and the one EIC channel in use belongs to the
       1PPS.  `RT_NOT_REACHABLE` is the TRUE answer here - the service knows
       the method, this device just has not configured the peripheral that
       way.  `RT_UNKNOWN_METHOD` would be a lie. */
    s_last_why = "no capture-capable input in the pin table";
    return EPSRV_RT_NOT_REACHABLE;
}

static uint8_t m_disable_event(const uint8_t *pl, uint16_t pl_len,
                               uint8_t *o, uint16_t cap, uint16_t *u)
{
    uint64_t v; bool f; uint16_t p = 0u;
    (void)o; (void)cap; (void)u;
    if (!wt_rd(pl, pl_len, &p, 0u, EPSRV_WT_U16, &v, &f) || !f) { return EPSRV_RT_NOT_REACHABLE; }
    {
        EPSRV_HANDLE *h = hnd_find((uint16_t)v);
        if (h == NULL) { return EPSRV_RT_NOT_REACHABLE; }
        /* Ending event generation here means: stop the signal, but KEEP the
           handle - `DisableGpioEvent` is not `ReleaseDigitalPins`. */
        PTP_TRIG_Cancel();
        PTP_TRIG_PulseSet(false, 0u);
    }
    return EPSRV_RT_OK;
}

/* Stubs.  They answer 0x05 ("peripheral not configured on that node"),
 * not 0x03 - the service KNOWS the method, the device just does not have
 * the peripheral.  A side effect that further justifies the path: if real
 * substance is added later, only the ANSWER of an already-existing method
 * changes, not the method list - a host that worked keeps working. */
static uint8_t m_stub(const uint8_t *pl, uint16_t pl_len,
                      uint8_t *o, uint16_t cap, uint16_t *u)
{
    (void)pl; (void)pl_len; (void)o; (void)cap; (void)u;
    s_st.stub_calls++;
    s_last_why = "stub: peripheral not configured";
    return EPSRV_RT_NOT_REACHABLE;
}

/* --------------------------------------------------------------------------- */
/* 5. Dispatch                                                                 */
/* --------------------------------------------------------------------------- */

typedef struct
{
    uint16_t   method;
    bool       silent;      /* fire-and-forget: MUST NOT reply                  */
    ep_handler fn;
} ep_entry_t;

/* The three fire-and-forget variants are a special case: a stub that sends
 * them 0x05 VIOLATES the protocol.  So recognise and stay silent - which
 * also means a host never learns for these methods that they are not
 * implemented.  That is a property of theirs, not a shortcoming of ours. */
static const ep_entry_t s_disp[] =
{
    { EPSRV_M_GET_STATUS,     false, m_get_status     },
    { EPSRV_M_RELEASE_PINS,   false, m_release_pins   },
    { EPSRV_M_OPEN_GPIO,      false, m_open_gpio      },
    { EPSRV_M_SET_GPIO,       false, m_set_gpio       },
    { EPSRV_M_GET_GPIO,       false, m_get_gpio       },
    { EPSRV_M_SET_GPIO_FAF,   true,  m_set_gpio_faf   },
    { EPSRV_M_ENABLE_PULSE,   false, m_enable_pulse   },
    { EPSRV_M_ENABLE_CAPTURE, false, m_enable_capture },
    { EPSRV_M_DISABLE_EVENT,  false, m_disable_event  },
    { EPSRV_M_OPEN_PWM,       false, m_open_pwm       },
    { EPSRV_M_CLOSE_PWM,      false, m_close_pwm      },
    { EPSRV_M_WRITE_PWM,      false, m_write_pwm      },
    { EPSRV_M_WRITE_PWM_FAF,  true,  m_write_pwm      },

    /* I2C - ids from some_ip.md 8.11.2, which is the source; deliberately NO
       second list of explanations here, just the numbers. */
    { 0x1200u, false, m_stub }, { 0x1202u, false, m_stub },
    { 0x1203u, false, m_stub }, { 0x1204u, false, m_stub },
    { 0x1206u, true,  m_stub }, { 0x1208u, false, m_stub },
    { 0x1220u, false, m_stub },
    /* UART */
    { 0x1400u, false, m_stub }, { 0x1404u, false, m_stub },
    { 0x1406u, true,  m_stub }, { 0x1420u, false, m_stub },
    /* SPI */
    { 0x1500u, false, m_stub }, { 0x1502u, false, m_stub },
    { 0x1508u, false, m_stub }, { 0x1509u, false, m_stub },
    { 0x1511u, false, m_stub },
    /* ADC.  0x1702 is simultaneously CloseAdc and ReadAdc for them, the
       proto does not know 0x1720 - as long as both answer 0x05, the
       question has no consequence.  It must be resolved BEFORE ADC gets
       real substance. */
    { 0x1700u, false, m_stub }, { 0x1702u, false, m_stub },
    { 0x1720u, false, m_stub }, { 0x1703u, false, m_stub },
    { 0x1704u, false, m_stub },
};

#define DISP_COUNT (sizeof(s_disp) / sizeof(s_disp[0]))

/* --------------------------------------------------------------------------- */
/* 6. Socket, receive, reply                                                   */
/* --------------------------------------------------------------------------- */

static void ep_reply(const ep_hdr_t *h, uint8_t rt, const uint8_t *pl, uint16_t pl_len,
                     IP_MULTI_ADDRESS *to, UDP_PORT port)
{
    UDP_SOCKET s;
    uint16_t total = (uint16_t)(EPSRV_HDR_LEN + pl_len);

    (void)hdr_build(s_tx, h, EPSRV_MSGTYPE_RESPONSE, rt);
    if (pl_len != 0u && pl != s_tx + EPSRV_HDR_LEN)
    {
        memcpy(s_tx + EPSRV_HDR_LEN, pl, pl_len);
    }
    put_be32(s_tx + 4, (uint32_t)(total - 8u));

    /* The reply's SOURCE PORT is free - the "fixed expectation 49153" from
       their documentation is not checked by the host (0 hits in their
       sources).  A client socket aimed at the requester is therefore the
       simplest correct way. */
    s = TCPIP_UDP_ClientOpen(IP_ADDRESS_TYPE_IPV4, port, to);
    if (s == INVALID_UDP_SOCKET) { s_st.tx_fail++; return; }
    if (TCPIP_UDP_PutIsReady(s) >= total
        && TCPIP_UDP_ArrayPut(s, s_tx, total) == total)
    {
        (void)TCPIP_UDP_Flush(s);
        s_st.tx_replies++;
    }
    else
    {
        s_st.tx_fail++;
    }
    TCPIP_UDP_Close(s);
}

static void ep_dispatch(uint16_t len, IP_MULTI_ADDRESS *from, UDP_PORT port)
{
    ep_hdr_t h;
    const uint8_t *pl;
    uint16_t pl_len, used = 0u;
    uint8_t rt;

    s_st.rx_frames++;
    if (!hdr_parse(s_rx, len, &h)) { s_st.rx_bad++; return; }

    /* DISCARD, do not evaluate.  Evaluating a foreign layout under a known
       service is the path by which a protocol fault turns into a pin fault
       (rcp.c:424). */
    if (h.service != EPSRV_SERVICE_ID || h.proto != EPSRV_PROTO_VER
        || h.iface != EPSRV_IFACE_VER)
    {
        s_st.rx_bad++;
        if (s_log)
        {
            SYS_CONSOLE_PRINT("[EP] dropped: svc %04X proto %u iface %u\r\n",
                              h.service, h.proto, h.iface);
        }
        return;
    }
    if (h.msgtype != EPSRV_MSGTYPE_REQUEST && h.msgtype != EPSRV_MSGTYPE_REQ_NORET)
    {
        s_st.rx_bad++;
        return;
    }

    pl     = s_rx + EPSRV_HDR_LEN;
    pl_len = (uint16_t)(h.length - 8u);
    s_st.last_method  = h.method;
    s_st.last_session = h.session;

    for (unsigned i = 0u; i < DISP_COUNT; i++)
    {
        if (s_disp[i].method != h.method) { continue; }
        s_last_why = "-";
        rt = s_disp[i].fn(pl, pl_len, s_tx + EPSRV_HDR_LEN,
                          (uint16_t)(EPSRV_BUF_MAX - EPSRV_HDR_LEN), &used);
        s_st.last_retcode = rt;
        if (s_log)
        {
            SYS_CONSOLE_PRINT("[EP] %04X sid %u -> %02X %s%s\r\n", h.method, h.session,
                              rt, (rt == EPSRV_RT_OK) ? "ok" : s_last_why,
                              s_disp[i].silent ? "  (silent)" : "");
        }
        /* Silent means silent - even on an error.  Fire-and-forget is not
           reply suppression in the good case, it is the absence of a
           return path. */
        if (s_disp[i].silent || h.msgtype == EPSRV_MSGTYPE_REQ_NORET)
        {
            s_st.silent++;
            return;
        }
        ep_reply(&h, rt, s_tx + EPSRV_HDR_LEN, used, from, port);
        return;
    }

    s_st.unknown_method++;
    s_st.last_retcode = EPSRV_RT_UNKNOWN_METHOD;
    if (s_log) { SYS_CONSOLE_PRINT("[EP] %04X unknown method\r\n", h.method); }
    /* An unknown method is ANSWERED, not stayed silent about: the host
       should be able to see the difference between "unknown" and "dead". */
    ep_reply(&h, EPSRV_RT_UNKNOWN_METHOD, NULL, 0u, from, port);
}

/* --------------------------------------------------------------------------- */
/* 6a. Service Discovery: the cyclic OfferService                              */
/* --------------------------------------------------------------------------- */

/* The frame, derived from the vendor sources (someip-gen.c
 * Fill_SomeIP_SD_Header / _Service_Header / _OptionIpV4_Header):
 *
 *   16  SOME/IP header: service 0xFFFF, method 0x8100, msgtype NOTIFICATION
 *    8  SD header:      flags(1) + reserved(3) + length of entries(4)
 *   16  entry:          type 1 = OfferService, option indices, service,
 *                       instance, major, TTL(24 bit), minor
 *    4  length of options
 *   12  IPv4 option:    inner length 9, type 0x04, address, protocol 0x11
 *                       (UDP), port
 *   = 56 bytes
 *
 * WHY THE ADDRESS IN THE OPTION IS WHAT COUNTS, NOT THE SENDER: the host
 * takes from the option where to send the methods.  If it holds 0.0.0.0, it
 * talks to nobody - and silently so, because the offer itself is formally
 * correct.  Hence the address is fetched fresh from the stack at send time,
 * and the offer is withheld as long as there is none. */
#define EPSRV_SD_FRAME_LEN      56u

static UDP_SOCKET s_sd_sock = INVALID_UDP_SOCKET;
static bool       s_offer = true;
static uint64_t   s_sd_due;
static uint32_t   s_sd_sent;
static uint32_t   s_sd_fail;
static uint16_t   s_sd_session = 1u;

static bool sd_open(void)
{
    IP_MULTI_ADDRESS to;

    if (s_sd_sock != INVALID_UDP_SOCKET) { return true; }
    /* 224.0.0.1 = all hosts.  Byte by byte, so nobody needs to know
       IPV4_ADDR.Val's endianness. */
    to.v4Add.v[0] = 224u; to.v4Add.v[1] = 0u;
    to.v4Add.v[2] = 0u;   to.v4Add.v[3] = 1u;
    s_sd_sock = TCPIP_UDP_ClientOpen(IP_ADDRESS_TYPE_IPV4, EPSRV_SD_PORT, &to);
    return (s_sd_sock != INVALID_UDP_SOCKET);
}

static void sd_send_offer(void)
{
    uint8_t f[EPSRV_SD_FRAME_LEN];
    uint32_t own = 0u;
    uint8_t *e = f + EPSRV_HDR_LEN + 8u;      /* the entry                     */
    uint8_t *o = e + 16u + 4u;                /* the IPv4 option               */

    {
        TCPIP_NET_HANDLE net = TCPIP_STACK_IndexToNet(EPSRV_IF);
        IPV4_ADDR ip;
        ip.Val = (net != NULL) ? TCPIP_STACK_NetAddress(net) : 0u;
        own = ((uint32_t)ip.v[0] << 24) | ((uint32_t)ip.v[1] << 16)
            | ((uint32_t)ip.v[2] << 8)  | (uint32_t)ip.v[3];
    }
    /* NO address, NO offer.  An offer with 0.0.0.0 in the option lets the
       host talk into the void, and silently so. */
    if (own == 0u) { return; }

    memset(f, 0, sizeof f);
    put_be16(f, EPSRV_SD_SERVICE);
    put_be16(f + 2, EPSRV_SD_METHOD);
    put_be32(f + 4, (uint32_t)(EPSRV_SD_FRAME_LEN - 8u));
    put_be16(f + 8, 0u);                      /* client id: SD has none         */
    put_be16(f + 10, s_sd_session++);
    if (s_sd_session == 0u) { s_sd_session = 1u; }
    f[12] = EPSRV_PROTO_VER;
    f[13] = EPSRV_SD_IFACE_VER;
    f[14] = EPSRV_MSGTYPE_NOTIFICATION;
    f[15] = EPSRV_RT_OK;

    /* SD header: flags 0.  The reboot bit stays off - setting it claims a
       restart, and a host may then discard its state on that basis. */
    f[EPSRV_HDR_LEN] = 0u;
    put_be32(f + EPSRV_HDR_LEN + 4u, 16u);    /* exactly one entry               */

    e[0] = 1u;                                /* SDServiceType_OfferService     */
    e[1] = 0u;                                /* index of the first option      */
    e[2] = 0u;
    e[3] = (uint8_t)(1u << 4);                /* one option in the first set    */
    put_be16(e + 4, EPSRV_SERVICE_ID);
    put_be16(e + 6, EPSRV_INSTANCE_ID);
    e[8] = EPSRV_SD_MAJOR;
    e[9]  = (uint8_t)(EPSRV_SD_TTL >> 16);
    e[10] = (uint8_t)(EPSRV_SD_TTL >> 8);
    e[11] = (uint8_t)EPSRV_SD_TTL;
    put_be32(e + 12, EPSRV_SD_MINOR);

    put_be32(e + 16, 12u);                    /* length of the options          */

    put_be16(o, 9u);                          /* innere Laenge                  */
    o[2] = 0x04u;                             /* IPv4-Endpunkt                  */
    o[3] = 0u;
    put_be32(o + 4, own);
    o[8] = 0u;
    o[9] = 0x11u;                             /* UDP                            */
    put_be16(o + 10, EPSRV_UDP_PORT);

    if (!sd_open()) { s_sd_fail++; return; }
    if (TCPIP_UDP_PutIsReady(s_sd_sock) >= sizeof f
        && TCPIP_UDP_ArrayPut(s_sd_sock, f, (uint16_t)sizeof f) == (uint16_t)sizeof f)
    {
        (void)TCPIP_UDP_Flush(s_sd_sock);
        s_sd_sent++;
    }
    else
    {
        s_sd_fail++;
    }
}

void EPSRV_OfferSet(bool on)
{
    s_offer = on;
    s_sd_due = 0u;                            /* at once, not only in 1 s       */
}

bool EPSRV_OfferGet(void)
{
    return s_offer;
}

void EPSRV_Initialize(void)
{
    memset(&s_st, 0, sizeof s_st);
    memset(s_hnd, 0, sizeof s_hnd);
    s_next_handle = 1u;
    s_sock = INVALID_UDP_SOCKET;
    s_log = false;
    s_up_ticks = SYS_TIME_Counter64Get();
    s_ticks_per_ms = SYS_TIME_MSToCount(1u);
    if (s_ticks_per_ms == 0u) { s_ticks_per_ms = 1u; }
}

void EPSRV_Tasks(void)
{
    uint16_t avail;

    /* The socket is only opened once the stack is running, and then ONCE -
       hammering `ServerOpen()` from every pass is the expensive mistake
       `trig_cmd.c` already documents. */
    if (s_sock == INVALID_UDP_SOCKET)
    {
        TCPIP_NET_HANDLE net = TCPIP_STACK_IndexToNet(EPSRV_IF);
        if (net == NULL || !TCPIP_STACK_NetIsUp(net)) { return; }
        s_sock = TCPIP_UDP_ServerOpen(IP_ADDRESS_TYPE_IPV4, EPSRV_UDP_PORT, NULL);
        if (s_sock == INVALID_UDP_SOCKET) { return; }
        (void)TCPIP_UDP_OptionsSet(s_sock, UDP_OPTION_STRICT_PORT, (void *)false);
        s_st.sock_open = true;
        SYS_CONSOLE_PRINT("[EP] endpoint service %04X listening on udp/%u\r\n",
                          EPSRV_SERVICE_ID, EPSRV_UDP_PORT);
        return;
    }

    /* ONE frame per pass.  Two reasons, both learned the expensive way: a
       handler can arm the trigger, and the reply goes over a socket that
       takes buffers from the same pool as the data path. */
    /* The cyclic offer.  BEFORE reception, so it still runs even while
       requests keep coming in - an endpoint that stops offering itself
       under load vanishes for every host that has just started looking. */
    if (s_offer)
    {
        uint64_t now = SYS_TIME_Counter64Get();
        if (now >= s_sd_due)
        {
            sd_send_offer();
            s_sd_due = now + (uint64_t)EPSRV_SD_PERIOD_MS * s_ticks_per_ms;
        }
    }

    avail = TCPIP_UDP_GetIsReady(s_sock);
    if (avail == 0u) { return; }
    {
        UDP_SOCKET_INFO info;
        uint16_t take = (avail > EPSRV_BUF_MAX) ? EPSRV_BUF_MAX : avail;
        uint16_t got  = TCPIP_UDP_ArrayGet(s_sock, s_rx, take);
        IP_MULTI_ADDRESS from;
        UDP_PORT port = EPSRV_UDP_PORT;

        memset(&from, 0, sizeof from);
        if (TCPIP_UDP_SocketInfoGet(s_sock, &info))
        {
            from.v4Add = info.sourceIPaddress.v4Add;
            port = info.remotePort;
        }
        (void)TCPIP_UDP_Discard(s_sock);
        if (got >= EPSRV_HDR_LEN) { ep_dispatch(got, &from, port); }
        else { s_st.rx_bad++; }
    }
}

void EPSRV_StatusGet(EPSRV_STATUS *out)
{
    if (out != NULL) { *out = s_st; }
}

void EPSRV_LogSet(bool on)
{
    s_log = on;
}

void EPSRV_Print(void)
{
    SYS_CONSOLE_PRINT("[EP] service %04X inst %u on udp/%u: %s   log %s\r\n",
                      EPSRV_SERVICE_ID, EPSRV_INSTANCE_ID, EPSRV_UDP_PORT,
                      s_st.sock_open ? "open" : "NOT OPEN", s_log ? "on" : "off");
    SYS_CONSOLE_PRINT("     rx %lu (bad %lu)   replies %lu (fail %lu)   silent %lu\r\n",
                      (unsigned long)s_st.rx_frames, (unsigned long)s_st.rx_bad,
                      (unsigned long)s_st.tx_replies, (unsigned long)s_st.tx_fail,
                      (unsigned long)s_st.silent);
    SYS_CONSOLE_PRINT("     unknown method %lu   stub calls %lu\r\n",
                      (unsigned long)s_st.unknown_method,
                      (unsigned long)s_st.stub_calls);
    SYS_CONSOLE_PRINT("     last: method %04X  session %u  retcode %02X  why %s\r\n",
                      s_st.last_method, s_st.last_session, s_st.last_retcode,
                      s_last_why);
    SYS_CONSOLE_PRINT("     offer: %s   sent %lu (failed %lu)   every %u ms"
                      "   to 224.0.0.1:%u\r\n",
                      s_offer ? "on" : "OFF", (unsigned long)s_sd_sent,
                      (unsigned long)s_sd_fail, (unsigned)EPSRV_SD_PERIOD_MS,
                      (unsigned)EPSRV_SD_PORT);
    SYS_CONSOLE_PRINT("     handles open %u of %u\r\n",
                      s_st.handles_open, EPSRV_HANDLES_MAX);
    for (unsigned i = 0u; i < EPSRV_HANDLES_MAX; i++)
    {
        if (!s_hnd[i].open) { continue; }
        SYS_CONSOLE_PRINT("       h%u  pin %u  %s  dir %u  interval %lu ns  duty q31 %lu\r\n",
                          s_hnd[i].id, s_hnd[i].pin_index,
                          (s_hnd[i].kind == 1u) ? "pwm " : "gpio",
                          s_hnd[i].direction,
                          (unsigned long)s_hnd[i].interval_ns,
                          (unsigned long)s_hnd[i].duty_q31);
    }
}
