# SOES Development Notes

This document covers changes made in this fork and how to build and develop the `linux_sim` applications. It supplements the upstream `README.md` without modifying it.

## What This Fork Adds

The upstream SOES repo provides the core stack and several hardware-specific HALs. This fork adds:

- **`applications/linux_sim/`** — `sim_demo`: simulated drive-like slave. Uses a memory-backed ESC and a raw AF_PACKET socket to communicate with a real SOEM master — no EtherCAT hardware required.
- **`applications/linux_sim_analog/`** — `sim_analog`: simulated 4-channel analog I/O slave. Same HAL, different PDO layout and object dictionary. Product code 0x00000002.
- **`soes/hal/linux-sim/`** — Software ESC HAL: frame processor thread (`esc_ecat.c`), in-memory register file (`esc_hw.c`), EEPROM emulation (`esc_hw_eep.c`).
- **`Dockerfile` + `entrypoint.sh`** — Container image for CI integration tests and standalone deployment. Set `SLAVE_TYPE=sim_demo` (default) or `SLAVE_TYPE=sim_analog` to select which binary runs.
- **`deploy.sh` + `env`** — Deployment script and configuration template for deploying the slave to a remote Linux host and running it alongside the vPLC (SOEM master) for development testing.

## Standalone Deployment (development testing)

Both the vPLC (SOEM master) and the SOES slave must run on the same **real Linux host** (Raspberry Pi, VM, or bare-metal server).

When both processes run on the same host they cannot share a real NIC — raw frames sent by one process go to the NIC driver and do **not** loop back to a second `AF_PACKET` listener on the same interface. A kernel **veth pair** solves this: it is a virtual back-to-back cable entirely inside the Linux kernel.

```
┌──────────────────────────────────────────────────┐
│           Remote Linux host (RPi / VM)           │
│                                                  │
│   vPLC / SOEM master          SOES slave         │
│   network_interface:     ──── SLAVE_IFACE:        │
│   veth-master                 veth-slave          │
│         │                          │             │
│         └──────── veth pair ───────┘             │
│               (kernel virtual cable)             │
└──────────────────────────────────────────────────┘
```

### Quick start

```bash
cd SOES

# 1. Create your local config (never committed — contains credentials)
cp env env.local

# 2. Edit env.local — set at minimum:
#      REMOTE_HOST=user@<ip-of-linux-host>
#      PLATFORM=arm64   (or amd64 / armv7)
#      SLAVE_TYPE=sim_demo
vim env.local

# 3. One-shot: build image on Mac → transfer to remote → create veth pair → start slave
./deploy.sh sim-full-deploy

# 4. Tail slave logs
./deploy.sh sim-logs

# 5. On the remote host, start the vPLC bound to veth-master:
#      ETHERCAT_INTERFACE=veth-master ./ethercat-up.sh host
#    or set  network_interface: veth-master  in your EtherCAT XML slave config.

# 6. Tear down when done
./deploy.sh sim-stop
```

### deploy.sh command reference

| Command | Description |
|---|---|
| `sim-full-deploy` | Build image (Mac) → SCP to remote → create veth pair → start slave **[one shot]** |
| `sim-start` | Create veth pair on remote + start slave (image must already be deployed) |
| `sim-stop` | Stop slave container + remove veth pair on remote |
| `sim-logs` | Tail slave container logs on remote host |
| `build` | Build Docker image → local `.tar.gz` tarball |
| `deploy` | SCP tarball to remote + `docker load` |
| `run` | Start slave on a physical NIC (`SLAVE_IFACE` from env.local) |
| `stop` | Stop the plain remote slave container |
| `logs` | Tail the plain remote slave container logs |
| `status` | Show container status on remote |
| `full-deploy` | `build` + `deploy` + `run` for physical NIC bench testing |
| `shell` | Open an interactive SSH shell to the remote host |
| `clean` | Remove local build tarballs and Docker image |

### env.local configuration

| Variable | Description |
|---|---|
| `PLATFORM` | Target CPU: `arm64` (RPi 4/5 64-bit), `armv7` (RPi 3/4 32-bit), `amd64` (x86) |
| `REMOTE_HOST` | SSH target: `user@host` or `user@ip` |
| `REMOTE_PASS` | SSH password — leave empty to use key-based auth |
| `SLAVE_TYPE` | `sim_demo` or `sim_analog` |
| `WAIT_TIMEOUT` | Seconds to wait for the interface before giving up (default: 30) |
| `SLAVE_IFACE` | Physical NIC — only used by `run`/`full-deploy` (hardware bench). `sim-*` always uses `veth-slave`. |
| `IMAGE_NAME` | Docker image name (default: `soes-slave`) |

> **Note:** `env.local` is listed in `.gitignore` — it is never committed. Commit only `env` (the template).

## Building the Simulated Slave

### With CMake (recommended)

```bash
cd SOES
mkdir -p build && cd build
cmake -DSIM_VARIANT=ON ..
make -j$(nproc)
# Binaries: build/applications/linux_sim/sim_demo
#           build/applications/linux_sim_analog/sim_analog
```

### Using the legacy build_sim wrapper

```bash
cd SOES/build_sim
make
# Binaries: build_sim/applications/linux_sim/sim_demo
#           build_sim/applications/linux_sim_analog/sim_analog
```

### Docker

```bash
cd SOES
docker build -t soes-slave .
```

## Running the Slave

The slave requires a network interface to bind to. It needs `CAP_NET_RAW` (i.e., root or the capability).

```bash
# Direct
sudo ./sim_demo   eth0
sudo ./sim_analog eth0

# Docker — interface is injected externally by the CI orchestrator
docker run -d --name soes-demo   --network none --privileged \
  -e SLAVE_IFACE=eth_s -e SLAVE_TYPE=sim_demo   -e WAIT_TIMEOUT=30 soes-slave
docker run -d --name soes-analog --network none --privileged \
  -e SLAVE_IFACE=eth_s -e SLAVE_TYPE=sim_analog -e WAIT_TIMEOUT=30 soes-slave
```

`entrypoint.sh` polls until the specified interface appears, brings it up, then runs the binary selected by `SLAVE_TYPE`.

## Application Architecture

Both slaves share the `soes` library, compiled once with `applications/linux_sim/ecat_options.h` as the HAL configuration (set in `cmake/Linux.cmake`). `MAX_MAPPINGS_SM2/SM3` in that file must be large enough for the slave with the most PDO entries — currently 4 for `sim_analog`.

All state lives in the global `_Objects Obj` struct (defined in each application's `utypes.h`).

### sim_demo

### Process data (PDO)

| Direction | SM | Objects |
|-----------|-----|---------|
| RxPDO (master → slave) | SM2 @ 0x1100 | `Control` (uint8), `Setpoint` (int32) |
| TxPDO (slave → master) | SM3 @ 0x1180 | `Status` (uint8), `Value` (int32) |

`cb_get_inputs()` runs each PDO cycle: `Value = counter × Gain + Offset`, `Status = Control`.

### SDO parameters (CoE, object 0x8000)

| Sub-index | Name | Type | Default |
|-----------|------|------|---------|
| 0x8000:01 | Gain | uint32 | 1 |
| 0x8000:02 | Offset | uint32 | 0 |

### Mailbox

- `MBXSIZE = 128` bytes, 3 buffers
- SM0 @ 0x1000 (receive), SM1 @ 0x1080 (send)
- CoE enabled always; FoE enabled (`USE_FOE=1`); EoE disabled (`USE_EOE=0`)

### sim_analog

Models a 4-channel analog I/O module. Product code 0x00000002 (patched at runtime via `EEP_write`).

### Process data (PDO)

| Direction | SM | Objects |
|-----------|-----|---------|
| RxPDO (master → slave) | SM2 @ 0x1100 | `AO0..AO3` (4×int16, 8 bytes) |
| TxPDO (slave → master) | SM3 @ 0x1180 | `AI0..AI3` (4×int16, 8 bytes) |

`cb_get_inputs()`: AI0/AI1 output a sawtooth ramp scaled by Gain + Offset. AI2/AI3 echo AO0/AO1 (loopback).

### SDO parameters (CoE, object 0x8000)

| Sub-index | Name | Type | Default |
|-----------|------|------|---------|
| 0x8000:01 | Gain | uint32 | 1 |
| 0x8000:02 | Offset | int16 | 0 |

### Mailbox

- Same mailbox layout as sim_demo
- CoE enabled; FoE disabled (`USE_FOE=0`); EoE disabled (`USE_EOE=0`)

### EEPROM emulation

`esc_hw_eep.c` generates a complete SII EEPROM image at startup from the object dictionary. Optionally loads from / saves to `sii_eeprom.bin` in the working directory.

## Extending the Slave

### Adding a FoE write handler

To test FoE from the master, register an `foe_write` hook in `main.c`:

```c
static uint8_t foe_buf[65536];
static uint32_t foe_len = 0;

static int foe_open(char *name, uint32_t len, uint8_t op)  { foe_len = 0; return 1; }
static int foe_write(uint8_t *buf, uint32_t len)           { memcpy(foe_buf + foe_len, buf, len); foe_len += len; return 1; }
static int foe_close(void)                                  { printf("FoE: received %u bytes\n", foe_len); return 1; }

// In esc_cfg_t config:
.foe_open  = foe_open,
.foe_write = foe_write,
.foe_close = foe_close,
```

> Note: check the exact `esc_cfg_t` field names in `ecat_slv.h` — they vary slightly across SOES versions.

### Enabling EoE

In `ecat_options.h`, change `USE_EOE` to `1` and implement a TUN/TAP handler in `main.c` to route Ethernet frames.

### Adding DC sync0 application handling

The slave already advertises DC support (`ESC_features` bit 2 = 1). To synchronize the application loop with the DC sync0 pulse, set `esc_check_dc_handler` in `esc_cfg_t` to a function that reads `ESCREG_DCSYNCACT` and adjusts the process data timing.

## CI Integration

The slave Docker image is used as one half of the two-container integration test. See `../ci/` and `../README.md` for the full test setup.

The CI test (`ci_master_test`) verifies 6 things via the `otee_ethercat` wrapper API:
1. Slave discovery (configure_slave validates product codes)
2. IOmap layout (get_slave_offsets matches expected PDO sizes)
3. PDO exchange (cyclic thread; both slave counters/values update)
4. CoE SDO (write_sdo Gain write on both slaves)
5. Analog loopback (write_outputs AO / read_inputs AI roundtrip)
6. Distributed Clocks (start with enable_dc=true succeeds)

FoE is not exposed by the wrapper; it is exercised by `soem_raw_test` (select with `TEST_BINARY=soem_raw_test`).
