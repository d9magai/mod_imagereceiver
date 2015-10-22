mod_imagereceiver.la: mod_imagereceiver.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_imagereceiver.lo
DISTCLEAN_TARGETS = modules.mk
shared =  mod_imagereceiver.la
