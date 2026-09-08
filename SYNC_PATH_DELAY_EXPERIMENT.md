# Controlled SYNC path-delay experiment

This firmware extends the FCT2 `SYNC` request with an optional fifth field:

```text
FCT2|SYNC|<sync_id>|<master_t1_us>|<artificial_reply_delay_us>
```

The original four-field packet remains accepted and means zero artificial reply delay.

For the experiment, the controller captures `t1` before any artificial Master→ESP delay.
The ESP captures `t2` immediately after `recvfrom()`. It captures `t3` before the optional
reply delay, waits using FreeRTOS, then sends `SYNC_REPLY`. The controller captures `t4`
on receipt. Therefore the injected delays are included in the NTP-style path timing rather
than hidden as endpoint processing time.

The paired controller provides three ESP02 calibration modes while ESP01 stays a no-delay
control:

- NONE: 0 ms forward / 0 ms reverse
- SYMMETRIC: 250 ms forward / 250 ms reverse
- ASYMMETRIC: 250 ms forward / 0 ms reverse

Verification exchanges are always delay-free. This is deliberate: symmetric delay should
raise RTT by about 500 ms without materially biasing offset, while 250/0 asymmetric delay
should bias the applied Master-minus-local offset by about -125 ms. Delay-free verification
then exposes that residual bias instead of reproducing it.

The watchdog-safe deadline-task START scheduler and bad-IP rejection logic are unchanged.
