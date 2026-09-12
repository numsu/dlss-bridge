FROM sunshine-gamestream:local

USER root
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      libpulse0:i386 \
      libvdpau1:i386 \
      libopenal1:i386 \
      libsm6:i386 \
      libice6:i386 \
      libsdl2-2.0-0:i386 \
      libglx0:i386 \
      libegl1:i386 \
      libibus-1.0-5 \
      libnss3 \
      libnspr4 \
      libgtk2.0-0 \
      libxcb-res0 \
      libvdpau1 \
      lsof \
      scrot \
      imagemagick \
      pciutils \
 && rm -rf /var/lib/apt/lists/*

COPY runtime/nvidia-compat32/ /usr/lib/i386-linux-gnu/
RUN ln -sf libGLX_nvidia.so.580.178.04 /usr/lib/i386-linux-gnu/libGLX_nvidia.so.0 \
 && ln -sf libnvidia-allocator.so.580.178.04 /usr/lib/i386-linux-gnu/libnvidia-allocator.so.1 \
 && ldconfig \
 && ln -sf /home/gamer/.steam/debian-installation/ubuntu12_32/steam-runtime/usr/libexec/steam-runtime-tools-0/x86_64-linux-gnu-srt-launcher-service /usr/local/bin/steam-runtime-launcher-service
