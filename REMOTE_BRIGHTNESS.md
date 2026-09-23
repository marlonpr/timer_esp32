# Runtime panel brightness control

Both supported targets now accept a runtime brightness command from the Windows controller.

Wire command:

```text
FCT2|CMD|BRIGHTNESS|<CommandId>|<percent>|0
```

- `percent`: integer `0..100`; `0` blanks LED output without changing timer state.
- The command is unicast by the Windows app to each selected device.
- The device replies with the ordinary ACK packet using command type `BRIGHTNESS`.
- The setting is runtime-only in this revision. Reboot restores the build-time `CONFIG_FACTORY_DISPLAY_BRIGHTNESS` default.
- START/START_AT synchronization, countdown scheduling, RESET behavior, and the idle logo are unchanged.

Backend mapping:

- classic ESP32: percentage directly controls the existing OE LEDC duty.
- ESP32-S3: percentage is mapped to the existing Hub75Driver `0..255` brightness input.
