################################################################################
#
# knulli-capture: knulli-screenshot and knulli-record
#
################################################################################

KNULLI_CAPTURE_VERSION = v1.0
KNULLI_CAPTURE_SITE = $(call github,dopazz,knulli-capture,$(KNULLI_CAPTURE_VERSION))
KNULLI_CAPTURE_LICENSE = GPL-3.0+
KNULLI_CAPTURE_LICENSE_FILES = LICENSE
KNULLI_CAPTURE_DEPENDENCIES = zlib x264 opus vo-aacenc pulseaudio

define KNULLI_CAPTURE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		VERSION=$(KNULLI_CAPTURE_VERSION)
endef

define KNULLI_CAPTURE_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) \
		VERSION=$(KNULLI_CAPTURE_VERSION) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
