# ---- RTL-SDR Build (Osmocom) ----
FROM alpine:3.20 AS rtlsdr-builder

RUN apk add --no-cache \
    git \
    cmake \
    make \
    gcc \
    musl-dev \
    libusb-dev \
    pkgconfig

# Clone Osmocom rtl-sdr (no_eeprom_write, active maintenance branch)
# original source https://gitea.osmocom.org/sdr/rtl-sdr.git
RUN git clone https://github.com/gbozo/rtl-sdr.git /rtl-sdr \
    --depth=1 --branch master

RUN cmake -S /rtl-sdr -B /rtl-sdr/build \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_BUILD_TYPE=Release \
        -DINSTALL_UDEV_RULES=OFF \
    && cmake --build /rtl-sdr/build --parallel "$(nproc)" \
    && cmake --install /rtl-sdr/build
