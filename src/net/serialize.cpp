#include "net/serialize.h"

#include <cstring>

namespace kalshi {

#define EMIT(s) (std::memcpy(p, s, sizeof(s) - 1), p += sizeof(s) - 1)

size_t serialize_action(
    const Action& a,
    const char* ticker,
    char* out,
    size_t cap
) noexcept {

    if (cap < 256) return 0;

    char* p = out;

    auto emit_lit = [&](const char* s, size_t n) {
        std::memcpy(p, s, n);
        p += n;
    };

    auto emit_u64 = [&](uint64_t v) {
        if (v == 0) { *p++ = '0'; return; }
        char buf[20];
        int len = 0;
        while (v > 0) {
            buf[len++] = char('0' + (v % 10));
            v /= 10;
        }
        while (len > 0) *p++ = buf[--len];
    };

    auto emit_u8 = [&](uint8_t v) {
        if (v >= 10) *p++ = char('0' + v / 10);
        *p++ = char('0' + v % 10);
    };

    if (a.kind == ActionKind::PLACE) {
        EMIT("{\"action\":\"place\",\"ticker\":\"");
        emit_lit(ticker, strlen(ticker));
        EMIT("\",\"side\":\"");
        if (a.side == Side::YES) EMIT("yes");
        else EMIT("no");
        EMIT("\",\"price\":");
        emit_u8(a.price);
        EMIT(",\"qty\":");
        emit_u64(a.qty);
        EMIT(",\"client_id\":");
        emit_u64(a.client_id);
        EMIT("}");
    } 

    // {"action":"cancel","ticker":"DEMO-1","client_id":42}
    else if (a.kind == ActionKind::CANCEL) {
        EMIT("{\"action\":\"cancel\",\"ticker\":\"");
        emit_lit(ticker, strlen(ticker));
        EMIT("\",\"client_id\":");
        emit_u64(a.client_id);
        EMIT("}");
    }
    else return 0;
    return p - out;

}

}