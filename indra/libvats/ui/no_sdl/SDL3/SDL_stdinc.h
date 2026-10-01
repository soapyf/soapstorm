// Stops vats_ui from building if anything in it includes SDL (see ui/CMakeLists.txt).
#error "vats_ui must not use SDL: the viewer has none. Put it behind ui::Host (ui/host.h)."
