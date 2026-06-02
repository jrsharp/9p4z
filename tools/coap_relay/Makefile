# CoAP-to-9P relay for local testing of the cloud transport.
#
# Requires libcoap 3.x:
#   Ubuntu/Debian:  sudo apt install libcoap3-dev
#   macOS:          brew install libcoap
#   From source:    https://github.com/obgm/libcoap

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = coap_relay

# Try pkg-config first; fall back to bare -lcoap-3
COAP_CFLAGS := $(shell pkg-config --cflags libcoap-3 2>/dev/null)
COAP_LIBS   := $(shell pkg-config --libs   libcoap-3 2>/dev/null)
ifeq ($(COAP_LIBS),)
  COAP_LIBS = -lcoap-3
endif

$(TARGET): main.c
	$(CC) $(CFLAGS) $(COAP_CFLAGS) -o $@ $< $(COAP_LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
