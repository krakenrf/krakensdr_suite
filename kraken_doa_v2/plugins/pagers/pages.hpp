// What the POCSAG and FLEX receivers share: the page counters and the facts
// about the last page.
#pragma once

#include "kraken_plugin.hpp"

#include <string>

namespace pagers {

class Pages {
public:
    explicit Pages(kp::Host& host) : host_(host) {}

    // a page decoded: address = "POCSAG RIC 1234567 F3" / "FLEX capcode
    // 0012345", text = what the event shows after the address
    void page(bool flex, const std::string& address, const std::string& text) {
        (flex ? flex_ : pocsag_)++;
        host_.fact("Last address", address);
        host_.fact("Last message", text);
        host_.fact("Messages", std::to_string(pocsag_ + flex_) + " (POCSAG " + std::to_string(pocsag_) +
                                   ", FLEX " + std::to_string(flex_) + ")");
    }

private:
    kp::Host& host_;
    int pocsag_ = 0, flex_ = 0;
};

}  // namespace pagers
