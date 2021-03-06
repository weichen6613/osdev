#include <inc/lib.h>
#include <inc/graphics.h>

static void process_event(struct graphics_event *ev) {
	if (ev->type == EVENT_MOUSE_CLICK) {
		draw_square(ev->d.emc.x, ev->d.emc.y, 20);
	}
}

void umain(int argc, char **argv) {
	init_graphics();
	color_canvas(COLOR_CRIMSON);
	event_loop(process_event);
}
