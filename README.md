# MinerBridge

WT32-ETH01 firmware — bridges your Bitcoin miner's Ethernet port to your home WiFi via NAT.

## How it works

| Step | What happens |
|------|-------------|
| Flash & power on | LED blinks 4x, board creates **MinerBridge** WiFi AP |
| Connect to AP | Open network, no password |
| Open browser | Go to **http://192.168.4.1** |
| Enter WiFi details | Your home WiFi SSID + password → Save |
| Reboot | Board connects to your WiFi, Ethernet port becomes LAN |
| Plug miner in | Miner gets IP from 192.168.4.x range, full internet via NAT |

**LED:** off = no WiFi, solid on = WiFi connected and routing.

**Status page:** http://192.168.4.1 (accessible from the Ethernet/miner side after setup).

**Reconfigure:** Hit "Reconfigure" on the status page to re-enter setup mode.

---

## Build

Requires ESP-IDF v5.x.

```bash
. $IDF_PATH/export.sh
idf.py build
```

---

## Flash (WT32-ETH01 / ESP32)

Using Flash Download Tool or esptool.py:

| File | Offset |
|------|--------|
| `build/bootloader/bootloader.bin` | `0x1000` |
| `build/partition_table/partition-table.bin` | `0x8000` |
| `build/miner_bridge.bin` | `0x10000` |

> **Important:** Power the WT32-ETH01 from a proper 5V USB charger (not just the serial adapter) to avoid brownout resets.

```bash
esptool.py --chip esp32 --port COM3 --baud 460800 \
  write_flash \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/miner_bridge.bin
```

---

## Network layout (after setup)

```
[Internet]
    |
[Your router]  192.168.1.x
    |  (WiFi)
[WT32-ETH01]   STA: 192.168.1.x (DHCP from your router)
                ETH: 192.168.4.1 (static, DHCP server)
    |  (Ethernet cable)
[Miner]        192.168.4.2 (DHCP from this device)
```

The miner's traffic is NATed through the WT32's WiFi uplink IP.
The miner appears as a single client on your WiFi network.
