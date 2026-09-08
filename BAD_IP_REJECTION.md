# Bad-IP rejection test

Expected factory LAN:

- Network: `192.168.0.0/16`
- Netmask: `255.255.0.0`
- Gateway: `192.168.0.1`

A good DHCP lease produces a log similar to:

```text
Accepted factory LAN: ip=192.168.5.123 mask=255.255.0.0 gw=192.168.0.1
Listening for UDP commands on port 5000
```

A bad lease such as the alternate `10.45.0.0/16` network is rejected:

```text
Rejected DHCP lease: ip=10.45.0.175 mask=255.255.0.0 gw=10.45.0.245; expected 192.168.0.0/16 with gateway 192.168.0.1 (attempt=1)
UDP command service remains disabled; requesting a fresh DHCP lease
Retrying DHCP after rejected network lease (attempt=1)
```

The firmware does not set the network-ready event bit for a rejected lease, so the UDP
command service cannot start on the wrong LAN. It stops the DHCP client, clears the
rejected IPv4 information, waits one second, and starts DHCP again. If another bad
lease arrives, the process repeats automatically until a valid factory-LAN lease is
received.

The current branch also contains the watchdog-safe deadline-task START scheduler. Bad-IP rejection remains independent of the synchronization timing path.
