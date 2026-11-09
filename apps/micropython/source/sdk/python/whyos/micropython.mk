# The WhyOS SDK as the MicroPython module "whyos" (modwhyos.c): built into the
# store's MicroPython with USER_C_MODULES=<WhyOS>/sdk/python (sdapps/build.sh).
WHYOS_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD += $(WHYOS_MOD_DIR)/modwhyos.c
CFLAGS_USERMOD += -I$(WHYOS_MOD_DIR)/../../include -I$(WHYOS_MOD_DIR)/../../../libwb

# WOS_SBSIM=<WhyOS>/sandbox/test: a MicroPython for the computer whose
# module runs under the sandbox's simulation (whyos_testing.PyApp): only
# this module's calls go to sbsim, MicroPython's own do not.
ifneq ($(WOS_SBSIM),)
SRC_USERMOD += $(WOS_SBSIM)/sbsim.c
CFLAGS_USERMOD += -I$(WOS_SBSIM)/..
%/modwhyos.o: CFLAGS += -include $(WOS_SBSIM)/sbsim.h -I$(WOS_SBSIM)
%/sbsim.o: CFLAGS += -w
endif
