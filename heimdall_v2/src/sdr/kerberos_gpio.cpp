#include "kerberos_gpio.hpp"
#include "../core/config.hpp"

#ifndef __linux__
#include <iostream>

// No Raspberry Pi header GPIOs off Linux (a Windows PC has none to drive the
// CKOVAL switches with): init fails with the reason, and main falls back to
// plain --kerberos (manual calibration) like on a non-Pi Linux box.
bool kerberos_gpio_init() {
#ifdef _WIN32
    std::cerr << "Kerberos switches: Raspberry Pi header GPIOs are not available on this platform (Windows)"
              << std::endl;
#else
    std::cerr << "Kerberos switches: Raspberry Pi header GPIOs are not available on this platform" << std::endl;
#endif
    return false;
}

bool kerberos_gpio_active() { return false; }

void kerberos_gpio_set_noise_path(bool) {}

void kerberos_gpio_cleanup() {}

#else  // __linux__

#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {

// Line request fd (owns the claimed GPIO lines); -1 = inactive.
int line_fd = -1;
std::mutex gpio_mutex;

// Software copy of the two output values (bit 0 = ANT1, bit 1 = ANT2) - the
// only writer of these pins is this module, so it is authoritative. Saved
// selection is restored when the noise source turns off.
uint64_t current_bits = 0b01;   // idle: ANT1=1, ANT2=0
uint64_t saved_bits = 0b01;
bool noise_path_engaged = false;

bool is_raspberry_pi() {
    std::ifstream f("/proc/device-tree/model");
    if (!f) return false;
    std::string model((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    return model.find("Raspberry Pi") != std::string::npos;
}

// The 40-pin header GPIOs live on the SoC pin controller, whose chip label
// starts with "pinctrl-" (rp1 on Pi 5, bcm2711 on Pi 4, bcm2835 on older
// models). The /dev/gpiochipN numbering varies between kernels, so scan.
int open_header_gpiochip() {
    for (int n = 0; n < 16; n++) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/gpiochip%d", n);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        struct gpiochip_info info;
        std::memset(&info, 0, sizeof(info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 &&
            std::strncmp(info.label, "pinctrl-", 8) == 0) {
            std::cout << "Kerberos switches: using " << path << " ("
                      << info.label << ", " << info.lines << " lines)" << std::endl;
            return fd;
        }
        close(fd);
    }
    return -1;
}

// Push `bits` to the hardware. Caller holds gpio_mutex.
bool write_lines_locked(uint64_t bits) {
    struct gpio_v2_line_values values;
    std::memset(&values, 0, sizeof(values));
    values.mask = 0b11;   // both requested lines
    values.bits = bits;
    if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) < 0) {
        std::cerr << "Kerberos switches: GPIO write failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    current_bits = bits;
    return true;
}

}  // namespace

bool kerberos_gpio_init() {
    std::lock_guard<std::mutex> lock(gpio_mutex);
    if (line_fd >= 0) return true;

    if (!is_raspberry_pi()) {
        std::cerr << "Kerberos switches: not running on a Raspberry Pi "
                     "(/proc/device-tree/model)" << std::endl;
        return false;
    }

    int chip_fd = open_header_gpiochip();
    if (chip_fd < 0) {
        std::cerr << "Kerberos switches: no pinctrl gpiochip found under /dev "
                     "(is the user in the 'gpio' group?)" << std::endl;
        return false;
    }

    struct gpio_v2_line_request req;
    std::memset(&req, 0, sizeof(req));
    req.offsets[0] = KERBEROS_SW_GPIO_ANT1;
    req.offsets[1] = KERBEROS_SW_GPIO_ANT2;
    req.num_lines = 2;
    std::strncpy(req.consumer, "heimdall-kerberos", sizeof(req.consumer) - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    // Idle antenna selection driven from the moment the lines are claimed.
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0b01;  // ANT1=1, ANT2=0
    req.config.attrs[0].mask = 0b11;

    const int rc = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip_fd);
    if (rc < 0 || req.fd < 0) {
        std::cerr << "Kerberos switches: failed to claim GPIO "
                  << KERBEROS_SW_GPIO_ANT1 << "/" << KERBEROS_SW_GPIO_ANT2
                  << ": " << std::strerror(errno)
                  << " (line already in use?)" << std::endl;
        return false;
    }

    line_fd = req.fd;
    current_bits = 0b01;
    saved_bits = 0b01;
    noise_path_engaged = false;
    std::cout << "Kerberos switches: GPIO " << KERBEROS_SW_GPIO_ANT1
              << " (ANT1)=1, GPIO " << KERBEROS_SW_GPIO_ANT2
              << " (ANT2)=0 - antenna input 1 selected" << std::endl;
    return true;
}

bool kerberos_gpio_active() {
    std::lock_guard<std::mutex> lock(gpio_mutex);
    return line_fd >= 0;
}

void kerberos_gpio_set_noise_path(bool noise_on) {
    std::lock_guard<std::mutex> lock(gpio_mutex);
    if (line_fd < 0) return;

    if (noise_on) {
        if (!noise_path_engaged) {
            // Save the antenna selection so a future manual selection (e.g.
            // antenna input 2) survives a calibration, exactly like the V1
            // rtl_daq.c gpioRead/gpioWrite pairing did.
            saved_bits = current_bits;
            noise_path_engaged = true;
        }
        if (write_lines_locked(0b00))
            std::cout << "Kerberos switches: antennas DISCONNECTED (noise path)" << std::endl;
    } else {
        if (!noise_path_engaged) return;  // nothing to restore
        noise_path_engaged = false;
        if (write_lines_locked(saved_bits))
            std::cout << "Kerberos switches: antenna selection restored" << std::endl;
    }
}

void kerberos_gpio_cleanup() {
    std::lock_guard<std::mutex> lock(gpio_mutex);
    if (line_fd < 0) return;
    // Leave the hardware pointed at the antennas, not the noise source.
    noise_path_engaged = false;
    write_lines_locked(0b01);
    close(line_fd);
    line_fd = -1;
}

#endif  // __linux__
