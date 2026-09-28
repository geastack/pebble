// The counter-jsx example, by hand in Pebble C: title, count, and three
// buttons (- + Reset). Up/down move focus, select presses.
#include <pebble.h>

static Window *s_window;
static Layer *s_layer;
static int s_count;
static int s_focus;

typedef struct { const char *text; GRect frame; } Button;
static const Button s_buttons[] = {
  { "-", { { 28, 146 }, { 66, 44 } } },
  { "+", { { 106, 146 }, { 66, 44 } } },
  { "Reset", { { 48, 198 }, { 104, 30 } } },
};

static void draw(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Counter", fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK), GRect(0, 8, bounds.size.w, 40),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  char count[12];
  snprintf(count, sizeof(count), "%d", s_count);
  graphics_context_set_text_color(ctx, GColorTiffanyBlue);
  graphics_draw_text(ctx, count, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK), GRect(0, 70, bounds.size.w, 40),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  for (int i = 0; i < 3; i++) {
    const GRect frame = s_buttons[i].frame;
    graphics_context_set_stroke_color(ctx, i == s_focus ? GColorWhite : GColorTiffanyBlue);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_round_rect(ctx, frame, 6);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_buttons[i].text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(frame.origin.x, frame.origin.y + (frame.size.h - 30) / 2, frame.size.w, 30),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

static void up(ClickRecognizerRef r, void *c) { s_focus = (s_focus + 2) % 3; layer_mark_dirty(s_layer); }
static void down(ClickRecognizerRef r, void *c) { s_focus = (s_focus + 1) % 3; layer_mark_dirty(s_layer); }
static void select(ClickRecognizerRef r, void *c) {
  if (s_focus == 0) s_count--;
  else if (s_focus == 1) s_count++;
  else s_count = 0;
  layer_mark_dirty(s_layer);
}

static void clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up);
  window_single_click_subscribe(BUTTON_ID_DOWN, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select);
}

int main(void) {
  s_window = window_create();
  window_set_click_config_provider(s_window, clicks);
  window_stack_push(s_window, true);
  Layer *root = window_get_root_layer(s_window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
  app_event_loop();
  layer_destroy(s_layer);
  window_destroy(s_window);
}
