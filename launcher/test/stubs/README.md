Stand-ins for the ESP-IDF and board-support headers, so that the
hardware-facing `app_*.c` files can be COMPILE-CHECKED on a host.

They are not a port and nothing here is ever linked or run - see
`check_app_sources.sh`, which is the only thing that uses them. Each stub
declares exactly what the real header is used for and no more, so adding a
new IDF call means adding a line here, deliberately.
