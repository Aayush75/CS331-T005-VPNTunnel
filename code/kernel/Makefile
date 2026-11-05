# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
ifneq ($(VERSION).$(PATCHLEVEL),6.18)
$(error VPT1 requires Linux 6.18.y; target is $(KERNELRELEASE))
endif
obj-m += vpt1_foundation.o vpt1_api_probe.o vpt1_parser_test.o
obj-m += vpt1_sequence_test.o vpt1_aead_test.o
obj-m += vpt1_acl_test.o
obj-m += vpt1_flow_test.o
obj-m += vpt1_zone_test.o
obj-m += vpt1_generation_test.o
obj-m += vpt1_ip_test.o vpt1_fragment_test.o
ifdef VPT1_DEBUG
obj-m += vpt1_gwtest.o
obj-m += vpt1_policy_test.o
endif
vpt1_api_probe-y := probes/api_probe.o
vpt1_foundation-y := runtime/foundation.o runtime/gateway.o runtime/netlink.o runtime/snapshot.o runtime/datapath.o policy/acl.o crypto/generation.o crypto/sequence.o crypto/parser.o ip/ipv4.o ip/error.o ip/fragment.o
vpt1_parser_test-y := crypto/parser.o ../tests/unit/vpt1_parser_test.o
vpt1_sequence_test-y := crypto/sequence.o ../tests/unit/vpt1_sequence_test.o
vpt1_aead_test-y := ../tests/unit/vpt1_aead_test.o
vpt1_acl_test-y := policy/acl.o ../tests/unit/vpt1_acl_test.o
vpt1_flow_test-y := policy/flow_state.o ../tests/unit/vpt1_flow_test.o
vpt1_zone_test-y := policy/zone_pool.o ../tests/unit/vpt1_zone_test.o
vpt1_gwtest-y := runtime/gateway.o policy/acl.o crypto/generation.o crypto/sequence.o crypto/parser.o ip/ipv4.o ip/error.o ip/fragment.o ../tests/unit/vpt1_gateway_test.o
vpt1_ip_test-y := ip/ipv4.o ip/error.o ../tests/unit/vpt1_ip_test.o
vpt1_policy_test-y := runtime/gateway.o policy/acl.o policy/flow_state.o policy/zone_pool.o crypto/generation.o crypto/sequence.o crypto/parser.o ip/ipv4.o ip/error.o ip/fragment.o ../tests/unit/vpt1_policy_test.o
vpt1_generation_test-y := crypto/generation.o crypto/sequence.o crypto/parser.o ../tests/unit/vpt1_generation_test.o
vpt1_fragment_test-y := ip/fragment.o ../tests/unit/vpt1_fragment_test.o
ccflags-y += $(if $(VPT1_DEBUG),-DVPT1_DEBUG) -I$(src)/include -I$(src)/../tests/conformance
else
KDIR ?= /lib/modules/$(shell uname -r)/build
.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
