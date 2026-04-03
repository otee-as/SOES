#!/bin/bash
#
# deploy.sh — SOES Simulated EtherCAT Slave Deployment Script
#
# Builds the SOES slave Docker image for a target architecture and deploys
# it to a remote Linux host (e.g. Raspberry Pi).  Mirrors the workflow of
# the otee-ethercat master deploy.sh.
#
# Usage:
#   ./deploy.sh <command>
#
# Quick start — simulation on remote Linux host (RPi / VM / server):
#   cp env env.local               # create your local config
#   vim env.local                  # set REMOTE_HOST, PLATFORM, SLAVE_TYPE
#   ./deploy.sh sim-full-deploy    # build + transfer + veth + slave (one shot)
#
# Why run slave AND master on the same remote Linux host?
#   Docker Desktop on macOS does not expose AF_PACKET raw sockets to
#   containers, so neither SOEM nor SOES can send EtherCAT frames from Mac.
#   Everything must run on a real Linux kernel (RPi, VM, bare-metal server).
#   The veth pair is created ON THAT REMOTE HOST so both the vPLC (SOEM
#   master) and the SOES slave can communicate over the virtual cable.
#
# Commands — Simulation (slave + veth, on remote Linux host):
#   sim-full-deploy   build + deploy image + create veth + start slave  [ONE SHOT]
#   sim-start         Create veth pair on remote + start slave container
#   sim-stop          Stop slave container + remove veth pair on remote
#   sim-logs          Tail slave container logs on remote host
#
# Commands — Plain remote run (slave on physical NIC, e.g. real hardware bench):
#   build          Build the Docker image and save as a .tar.gz tarball
#   deploy         Transfer the tarball to remote and load it into Docker
#   run            Start (or restart) the slave container on the remote host
#   stop           Stop the slave container on the remote host
#   logs           Tail slave container logs (Ctrl+C to stop)
#   status         Show slave container status on the remote host
#   full-deploy    build + deploy + run  (complete one-shot workflow)
#   shell          Open an interactive SSH shell to the remote host
#   clean          Remove local build artifacts and Docker image
#
# Configuration (env.local):
#   PLATFORM       Target CPU architecture: arm64 | armv7 | amd64
#   REMOTE_HOST    SSH target in user@host format (e.g. pi@192.168.1.100)
#   REMOTE_PASS    SSH password (optional — leave empty to use key auth)
#   SLAVE_TYPE     sim_demo | sim_analog  (default: sim_demo)
#   IMAGE_NAME     Docker image name  (default: soes-slave)
#
#   SLAVE_IFACE    Physical NIC interface — used only by 'run' / 'full-deploy'
#                  (plain hardware bench mode).  sim-* commands always use
#                  veth-slave on the remote host, hardwired.

set -e

# ── Load configuration ────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -f "${SCRIPT_DIR}/env.local" ]; then
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/env.local"
    echo "Loaded configuration from env.local"
elif [ -f "${SCRIPT_DIR}/env" ]; then
    # shellcheck source=/dev/null
    source "${SCRIPT_DIR}/env"
    echo "Loaded configuration from env (consider creating env.local)"
fi

# ── Defaults (can be overridden by env.local) ─────────────────────────────────
PLATFORM="${PLATFORM:-arm64}"
REMOTE_HOST="${REMOTE_HOST:-pi@192.168.1.100}"
REMOTE_PASS="${REMOTE_PASS:-}"
SLAVE_IFACE="${SLAVE_IFACE:-eth0}"
SLAVE_TYPE="${SLAVE_TYPE:-sim_demo}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-30}"
IMAGE_NAME="${IMAGE_NAME:-soes-slave}"
IMAGE_TAG="${IMAGE_TAG:-latest}"

# deploy.sh lives inside the SOES repo root, so Dockerfile and build context
# are in the same directory as this script.
DOCKERFILE="${SCRIPT_DIR}/Dockerfile"
BUILD_CONTEXT="${SCRIPT_DIR}"
CONTAINER_NAME="soes-sim-slave"

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()  { echo -e "${BLUE}===> $*${NC}"; }
ok()   { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}WARN: $*${NC}"; }
err()  { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }

# ── SSH/SCP helpers ───────────────────────────────────────────────────────────
_ssh() {
    if [ -n "$REMOTE_PASS" ]; then
        sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no "$REMOTE_HOST" "$@"
    else
        ssh "$REMOTE_HOST" "$@"
    fi
}

_scp() {
    if [ -n "$REMOTE_PASS" ]; then
        sshpass -p "$REMOTE_PASS" scp -o StrictHostKeyChecking=no "$@"
    else
        scp "$@"
    fi
}

# ── Platform → Docker platform string ────────────────────────────────────────
docker_platform() {
    case "$PLATFORM" in
        amd64)        echo "linux/amd64" ;;
        arm64)        echo "linux/arm64" ;;
        armv7|arm32)  echo "linux/arm/v7"; PLATFORM="armv7" ;;
        *) err "Unknown platform: $PLATFORM  (supported: amd64, arm64, armv7)" ;;
    esac
}

tarball_name() { echo "${IMAGE_NAME}-${PLATFORM}.tar.gz"; }
tagged_image()  { echo "${IMAGE_NAME}-${PLATFORM}:${IMAGE_TAG}"; }

# ── Commands ──────────────────────────────────────────────────────────────────

cmd_build() {
    log "Building SOES slave image for platform: ${PLATFORM}..."

    if ! docker buildx version > /dev/null 2>&1; then
        err "Docker buildx not available. Install with: docker buildx create --name multiarch --use"
    fi

    DOCKER_PLATFORM="$(docker_platform)"

    docker buildx build \
        --platform "${DOCKER_PLATFORM}" \
        -t "$(tagged_image)" \
        -f "${DOCKERFILE}" \
        --load \
        "${BUILD_CONTEXT}"

    log "Saving image to tarball: $(tarball_name)..."
    docker save "$(tagged_image)" | gzip > "${SCRIPT_DIR}/$(tarball_name)"

    ok "Built and saved: $(tarball_name)"
    ls -lh "${SCRIPT_DIR}/$(tarball_name)"
}

cmd_deploy() {
    local tarball="${SCRIPT_DIR}/$(tarball_name)"
    [ -f "$tarball" ] || err "Tarball not found: $tarball  (run 'build' first)"

    log "Transferring image to ${REMOTE_HOST}..."
    _scp "$tarball" "${REMOTE_HOST}:~/"

    log "Loading image on remote host..."
    _ssh "gunzip -c ~/$(tarball_name) | docker load && rm -f ~/$(tarball_name)"

    ok "Image deployed to ${REMOTE_HOST}"
}

cmd_run() {
    log "Starting slave container on ${REMOTE_HOST}..."
    log "  Image:      $(tagged_image)"
    log "  Interface:  ${SLAVE_IFACE}"
    log "  Slave type: ${SLAVE_TYPE}"

    # Stop any stale instance first
    _ssh "docker stop ${CONTAINER_NAME} 2>/dev/null; docker rm -f ${CONTAINER_NAME} 2>/dev/null; true"

    _ssh "docker run -d \
        --name ${CONTAINER_NAME} \
        --restart unless-stopped \
        --network host \
        --cap-add NET_RAW \
        --cap-add NET_ADMIN \
        -e SLAVE_IFACE=${SLAVE_IFACE} \
        -e SLAVE_TYPE=${SLAVE_TYPE} \
        -e WAIT_TIMEOUT=${WAIT_TIMEOUT} \
        $(tagged_image)"

    ok "Slave container '${CONTAINER_NAME}' started"
    echo ""
    log "Showing first 20 log lines..."
    sleep 1
    _ssh "docker logs --tail 20 ${CONTAINER_NAME}" || true
    echo ""
    echo "  Tail logs:  ./deploy.sh logs"
    echo "  Stop slave: ./deploy.sh stop"
}

cmd_stop() {
    log "Stopping slave container on ${REMOTE_HOST}..."
    _ssh "docker stop ${CONTAINER_NAME} 2>/dev/null && docker rm ${CONTAINER_NAME} 2>/dev/null || true"
    ok "Container '${CONTAINER_NAME}' stopped"
}

cmd_logs() {
    log "Tailing logs from ${CONTAINER_NAME} on ${REMOTE_HOST} (Ctrl+C to stop)..."
    _ssh "docker logs -f ${CONTAINER_NAME}"
}

cmd_status() {
    log "Container status on ${REMOTE_HOST}:"
    _ssh "docker ps -a --filter name=${CONTAINER_NAME} --format 'table {{.Names}}\t{{.Status}}\t{{.Image}}'" || true
}

cmd_full_deploy() {
    echo ""
    echo -e "${GREEN}============================================${NC}"
    echo -e "${GREEN}  SOES Slave Full Deploy${NC}"
    echo -e "${GREEN}  Platform : ${PLATFORM}${NC}"
    echo -e "${GREEN}  Target   : ${REMOTE_HOST}${NC}"
    echo -e "${GREEN}  Interface: ${SLAVE_IFACE}${NC}"
    echo -e "${GREEN}  Type     : ${SLAVE_TYPE}${NC}"
    echo -e "${GREEN}============================================${NC}"
    echo ""
    cmd_build
    cmd_deploy
    cmd_run
}

cmd_shell() {
    log "Opening SSH shell to ${REMOTE_HOST}..."
    if [ -n "$REMOTE_PASS" ]; then
        sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no "$REMOTE_HOST"
    else
        ssh "$REMOTE_HOST"
    fi
}

cmd_clean() {
    log "Cleaning local artifacts..."
    rm -f "${SCRIPT_DIR}/${IMAGE_NAME}-"*.tar.gz
    docker rmi "$(tagged_image)" 2>/dev/null || true
    ok "Cleanup complete"
}

# ── Remote simulation helpers (veth pair + slave, both on remote Linux host) ──
#
# Why everything runs on the remote Linux host:
#   Docker Desktop on macOS does not pass AF_PACKET raw sockets through to
#   containers — SOEM and SOES frames never reach the wire.  All EtherCAT
#   work must happen on a real Linux kernel.
#
# Topology on the remote host:
#
#   ┌──────────────────────────────────────────────────┐
#   │              Remote Linux host (RPi / VM)        │
#   │                                                  │
#   │   vPLC / SOEM master          SOES slave         │
#   │   (network_interface:    ──── (SLAVE_IFACE:       │
#   │    veth-master)               veth-slave)        │
#   │         │                          │             │
#   │         └──────── veth pair ───────┘             │
#   │                  (kernel virtual                 │
#   │                   back-to-back cable)            │
#   └──────────────────────────────────────────────────┘
#
# The veth pair is created with sudo on the remote host via SSH.
# The slave container is started with --network host + NET_RAW / NET_ADMIN.
# SLAVE_IFACE is always hardwired to veth-slave for sim commands.

SIM_VETH_MASTER="veth-master"
SIM_VETH_SLAVE="veth-slave"
SIM_CONTAINER_NAME="soes-sim-slave"

cmd_sim_start() {
    log "Setting up simulation on remote host ${REMOTE_HOST}..."
    log "  Slave type: ${SLAVE_TYPE}"
    log "  veth pair:  ${SIM_VETH_MASTER} <-> ${SIM_VETH_SLAVE}"

    # Check the image is present on the remote host
    if ! _ssh "docker image inspect $(tagged_image) > /dev/null 2>&1"; then
        err "Image '$(tagged_image)' not found on ${REMOTE_HOST}.  Run 'deploy' first."
    fi

    # Create veth pair on the remote host (requires sudo there)
    _ssh "sudo bash -s" <<EOF
set -e
echo '--- Creating veth pair ---'
ip link del ${SIM_VETH_MASTER} 2>/dev/null || true
ip link add ${SIM_VETH_MASTER} type veth peer name ${SIM_VETH_SLAVE}
# EtherCAT uses raw Ethernet (EtherType 0x88A4) — no IP needed
ip link set ${SIM_VETH_MASTER} promisc on up
ip link set ${SIM_VETH_SLAVE}  promisc on up
echo 'veth pair UP'
EOF
    ok "veth pair created on ${REMOTE_HOST}"

    # Stop any stale slave container
    _ssh "docker stop ${SIM_CONTAINER_NAME} 2>/dev/null; docker rm -f ${SIM_CONTAINER_NAME} 2>/dev/null; true"

    # Start the slave container — SLAVE_IFACE is always veth-slave for sim
    _ssh "docker run -d \
        --name ${SIM_CONTAINER_NAME} \
        --restart unless-stopped \
        --network host \
        --cap-add NET_RAW \
        --cap-add NET_ADMIN \
        -e SLAVE_IFACE=${SIM_VETH_SLAVE} \
        -e SLAVE_TYPE=${SLAVE_TYPE} \
        -e WAIT_TIMEOUT=${WAIT_TIMEOUT} \
        $(tagged_image)"

    sleep 1
    _ssh "docker logs --tail 20 ${SIM_CONTAINER_NAME}" || true

    echo ""
    ok "Simulation slave is running on ${REMOTE_HOST}:${SIM_VETH_SLAVE}"
    echo ""
    echo "  On the remote host, start the vPLC bound to ${SIM_VETH_MASTER}:"
    echo "    ETHERCAT_INTERFACE=veth-master ./ethercat-up.sh host"
    echo "    or set  network_interface: veth-master  in your EtherCAT XML config"
    echo ""
    echo "  Tail slave logs:  ./deploy.sh sim-logs"
    echo "  Tear down:        ./deploy.sh sim-stop"
}

cmd_sim_stop() {
    log "Tearing down simulation on ${REMOTE_HOST}..."

    _ssh "docker stop ${SIM_CONTAINER_NAME} 2>/dev/null; docker rm -f ${SIM_CONTAINER_NAME} 2>/dev/null; true"

    _ssh "sudo bash -s" <<EOF
ip link del ${SIM_VETH_MASTER} 2>/dev/null || true
echo 'veth pair removed'
EOF

    ok "Simulation stack torn down on ${REMOTE_HOST}"
}

cmd_sim_logs() {
    log "Tailing simulation slave logs on ${REMOTE_HOST} (Ctrl+C to stop)..."
    _ssh "docker logs -f ${SIM_CONTAINER_NAME}"
}

cmd_sim_full_deploy() {
    echo ""
    echo -e "${GREEN}============================================${NC}"
    echo -e "${GREEN}  SOES Slave — Simulation Full Deploy${NC}"
    echo -e "${GREEN}  Platform  : ${PLATFORM}${NC}"
    echo -e "${GREEN}  Target    : ${REMOTE_HOST}${NC}"
    echo -e "${GREEN}  Slave type: ${SLAVE_TYPE}${NC}"
    echo -e "${GREEN}  veth      : ${SIM_VETH_MASTER} <-> ${SIM_VETH_SLAVE}${NC}"
    echo -e "${GREEN}============================================${NC}"
    echo ""
    cmd_build
    cmd_deploy
    cmd_sim_start
}

# ── sshpass check ─────────────────────────────────────────────────────────────
if ! command -v sshpass &>/dev/null && [ -n "$REMOTE_PASS" ]; then
    warn "sshpass not installed — you'll be prompted for password interactively."
    warn "  macOS: brew install hudochenkov/sshpass/sshpass"
    warn "  Linux: apt-get install sshpass"
    REMOTE_PASS=""
fi

# ── Dispatch ──────────────────────────────────────────────────────────────────
case "${1:-}" in
    build)        cmd_build ;;
    deploy)       cmd_deploy ;;
    run)          cmd_run ;;
    stop)         cmd_stop ;;
    logs)         cmd_logs ;;
    status)       cmd_status ;;
    full-deploy)  cmd_full_deploy ;;
    shell)        cmd_shell ;;
    clean)        cmd_clean ;;
    sim-full-deploy)  cmd_sim_full_deploy ;;
    sim-start)        cmd_sim_start ;;
    sim-stop)         cmd_sim_stop ;;
    sim-logs)         cmd_sim_logs ;;
    *)
        echo ""
        echo "Usage: $0 <command>"
        echo ""
        echo "Simulation commands (slave + veth pair on remote Linux host — for testing vPLC master):"
        echo "  sim-full-deploy   build + deploy image + create veth + start slave  [ONE SHOT]"
        echo "  sim-start         Create veth pair on remote + start slave container"
        echo "  sim-stop          Stop slave + remove veth pair on remote"
        echo "  sim-logs          Tail slave container logs on remote host"
        echo ""
        echo "Plain remote commands (slave on physical NIC, real hardware bench):"
        echo "  build             Build Docker image → tarball"
        echo "  deploy            Transfer + load tarball on remote host"
        echo "  run               Start slave container on remote host"
        echo "  stop              Stop slave container on remote host"
        echo "  logs              Tail slave container logs"
        echo "  status            Show container status on remote host"
        echo "  full-deploy       build + deploy + run"
        echo "  shell             Open SSH shell to remote host"
        echo "  clean             Remove local build artifacts"
        echo ""
        echo "Configuration: copy env → env.local and set REMOTE_HOST, PLATFORM, SLAVE_TYPE"
        echo ""
        exit 1
        ;;
esac
