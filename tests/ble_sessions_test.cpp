// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/ble_sessions.h"
#include <cassert>
int main() {
    codex_micro::BleSessions sessions;
    const uint8_t a[6] = {1,2,3,4,5,6}, b[6] = {6,5,4,3,2,1};
    const auto a1=sessions.connect(1,a);
    assert(a1 != 0 && sessions.token(1,a)==a1);
    const auto b1=sessions.connect(2,b);
    assert(b1 != a1 && sessions.token(1,a)==a1);
    assert(sessions.disconnect(2,b)==b1);
    assert(sessions.connect(2,b)!=b1 && sessions.token(1,a)==a1);
    assert(sessions.disconnect(1,b)==0 && sessions.token(1,a)==a1);
    assert(sessions.disconnect(1,a)==a1 && sessions.token(1,a)==0);
    const auto a2=sessions.connect(1,a);
    assert(a2!=a1 && sessions.token(1,a)==a2); // queued old write now rejected
    assert(sessions.connect(1,a)==a2); // duplicate connect callback
    const auto replacement=sessions.connect(1,b);
    assert(replacement!=a2 && sessions.token(1,a)==0);
    assert(sessions.disconnect(1,a)==0 && sessions.token(1,b)==replacement);
}
