# Vendored-component patches

`esp_tinyusb` and `tinyusb` are ESP-IDF **managed components** — the component
manager downloads them (pristine) into the gitignored `managed_components/`, so a
source fix in them can't just be committed with the rest of the tree. This folder
re-applies the one fix Falcon needs.

## `apply_tinyusb_patch.py` — dwc2 EP0-SETUP-DMA crash fix

**What:** clamps the SETUP-packet pointer in the ESP32-S3 dwc2 device driver
(`tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c`).

**Why:** on the ESP32-S3, reading `DOEPDMA0` to locate the last SETUP packet can
return a garbage pointer when a SETUP overlaps heavy isochronous-IN traffic; the
following `memcpy` in `dcd_event_setup_received` then faults (LoadProhibited).
The SETUP packet is always DMA'd into `_dcd_usbbuf.setup_buffer`, so the fix
clamps the pointer to that buffer. **Without it, DMA mode crashes ~2–3 s into
streaming on the Xbox** (the app's continuous EP0 control traffic overlaps our
iso stream). DMA mode is required — SLAVE/FIFO mode truncates large FS-iso
packets on this PHY.

**Applied automatically:** `main/CMakeLists.txt` runs this script every configure
(via `execute_process`), so a normal `idf.py build` picks it up. It is idempotent
(a no-op once the `FALCON FIX` marker is present) and version-tolerant (it targets
the single setup-packet line by content, not by line number).

**Run by hand** (e.g. after `idf.py fullclean` or a dependency refresh):

```
python patches/apply_tinyusb_patch.py           # from the project root
```

If tinyusb ever changes that line and the script reports it can't find the target,
apply the change shown in the script's `REPLACEMENT` block manually to
`managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c`
(replace the `tusb_control_request_t *setup_packet = … epout0->doepdma …;` line).
