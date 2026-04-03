#!/bin/bash
# entrypoint.sh — SOES simulated EtherCAT slave entrypoint
#
# Waits for a network interface to be injected into this container's
# network namespace (by an external orchestrator), then runs the
# selected slave binary.
#
# Environment variables:
#   SLAVE_IFACE   Network interface to listen on (default: eth_s)
#   WAIT_TIMEOUT  Seconds to wait for interface (default: 30)
#   SLAVE_TYPE    Which slave to run: sim_demo | sim_analog (default: sim_demo)

set -euo pipefail

IFACE="${SLAVE_IFACE:-eth_s}"
TIMEOUT="${WAIT_TIMEOUT:-30}"
SLAVE_TYPE="${SLAVE_TYPE:-sim_demo}"

echo "=== SOES Simulated Slave ==="
echo "Slave type:  ${SLAVE_TYPE}"
echo "Waiting for interface '${IFACE}' (up to ${TIMEOUT}s)..."

max_iter=$(( TIMEOUT * 5 ))
iter=0
while ! ip link show "${IFACE}" &>/dev/null; do
    sleep 0.2
    iter=$(( iter + 1 ))
    if [ "$iter" -ge "$max_iter" ]; then
        echo "ERROR: Interface '${IFACE}' did not appear within ${TIMEOUT}s"
        ip link show
        exit 1
    fi
done

echo "Interface '${IFACE}' found, bringing up..."
ip link set "${IFACE}" up
ip link set "${IFACE}" promisc on
ip link set "${IFACE}" txqueuelen 1000
echo "Interface ready."

rm -f sii_eeprom.bin

case "${SLAVE_TYPE}" in
    sim_demo|sim_analog)
        ;;
    *)
        echo "ERROR: Unknown SLAVE_TYPE '${SLAVE_TYPE}'"
        exit 1
        ;;
esac

echo "Starting ${SLAVE_TYPE} on '${IFACE}'..."
exec "./${SLAVE_TYPE}" "${IFACE}"
