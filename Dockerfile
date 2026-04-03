# SOES Simulated EtherCAT Slave
#
# Standalone Docker image for the SOES linux_sim application.
# The container waits for a network interface to be injected into its
# network namespace before starting the simulated slave.
#
# Build:
#   docker build -t soes-slave .
#
# Run (interface must be injected externally by an orchestrator):
#   docker run -d --name soes-slave --network none --privileged \
#     -e SLAVE_IFACE=eth_s -e WAIT_TIMEOUT=30 soes-slave

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    iproute2 \
    wget \
    ca-certificates \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

# CMake 3.28+ from Kitware (SOEM requires it)
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | \
    gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null && \
    echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' | \
    tee /etc/apt/sources.list.d/kitware.list >/dev/null && \
    apt-get update && apt-get install -y cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build/soes
COPY CMakeLists.txt version.h.in LICENSE ./
COPY cmake/ cmake/
COPY soes/ soes/
COPY applications/linux_sim/ applications/linux_sim/
COPY applications/linux_sim_analog/ applications/linux_sim_analog/

RUN mkdir -p build && cd build \
    && cmake -DSIM_VARIANT=ON -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_POLICY_VERSION_MINIMUM=3.5 .. \
    && cmake --build . -j"$(nproc)"

RUN mkdir -p /app && \
    cp /build/soes/build/applications/linux_sim/sim_demo /app/ && \
    cp /build/soes/build/applications/linux_sim_analog/sim_analog /app/

WORKDIR /app

COPY entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]
