# name of your application
APPLICATION = soletta_app

# If no BOARD is found in the environment, use this default:
BOARD ?= samr21-xpro

# This has to be the absolute path to the RIOT base directory:
RIOTBASE ?= $(CURDIR)/../..

# Comment this out to disable code in RIOT that does safety checking
# which is not needed in a production environment but helps in the
# development process:
CFLAGS += -DDEVELHELP

# Change this to 0 show compiler invocation lines by default:
QUIET ?= 1

USEPKG += libsoletta
USEMODULE += posix
USEMODULE += gnrc_netif_default
USEMODULE += auto_init_gnrc_netif
USEMODULE += gnrc_ipv6_default
USEMODULE += gnrc_udp
USEMODULE += ps

CFLAGS += -D__LINUX_ERRNO_EXTENSIONS__=1 -DTHREAD_STACKSIZE_MAIN=3072

include $(RIOTBASE)/Makefile.include
