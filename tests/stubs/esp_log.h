#pragma once

// ContextLink's logs are irrelevant to host-side state/transport tests.  Keep
// every argument type-checked and marked used while avoiding an ESP-IDF
// dependency.  A single variadic parameter also supports calls with no format
// arguments without relying on the non-standard `, ##__VA_ARGS__` extension.
template <typename... Args>
inline void EspLogTestSink(Args&&...)
{
}

#define ESP_LOGE(...) EspLogTestSink(__VA_ARGS__)
#define ESP_LOGI(...) EspLogTestSink(__VA_ARGS__)
#define ESP_LOGW(...) EspLogTestSink(__VA_ARGS__)
