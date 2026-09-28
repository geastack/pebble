// The counter-jsx example, by hand in Alloy + Poco: title, count, and three
// buttons (- + Reset). Up/down move focus, select presses.
import Poco from "commodetto/Poco";
import Button from "pebble/button";

const render = new Poco(screen);
const title = new render.Font("Bitham-Black", 30);
const label = new render.Font("Gothic-Bold", 24);
const black = render.makeColor(0, 0, 0);
const white = render.makeColor(255, 255, 255);
const teal = render.makeColor(0, 170, 170);

const buttons = [
	{ text: "-", x: 28, y: 146, w: 66, h: 44 },
	{ text: "+", x: 106, y: 146, w: 66, h: 44 },
	{ text: "Reset", x: 48, y: 198, w: 104, h: 30 }
];
let count = 0;
let focus = 0;

function centered(text, font, color, y) {
	render.drawText(text, font, color, (render.width - render.getTextWidth(text, font)) / 2, y);
}

function draw() {
	render.begin();
	render.fillRectangle(black, 0, 0, render.width, render.height);
	centered("Counter", title, white, 8);
	centered(String(count), title, teal, 70);
	buttons.forEach((b, i) => {
		render.fillRectangle(i === focus ? white : teal, b.x, b.y, b.w, b.h);
		render.fillRectangle(black, b.x + 2, b.y + 2, b.w - 4, b.h - 4);
		render.drawText(b.text, label, white, b.x + (b.w - render.getTextWidth(b.text, label)) / 2, b.y + (b.h - label.height) / 2);
	});
	render.end();
}

new Button({
	types: ["up", "down", "select"],
	onPush(pushed, type) {
		if (!pushed) return;
		if (type === "up") focus = (focus + 2) % 3;
		else if (type === "down") focus = (focus + 1) % 3;
		else if (focus === 0) count--;
		else if (focus === 1) count++;
		else count = 0;
		draw();
	}
});
draw();
