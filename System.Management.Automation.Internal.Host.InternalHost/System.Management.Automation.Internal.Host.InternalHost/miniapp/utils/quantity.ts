import type { InventoryItem } from "../types/models";

function cleanQuantityText(value: unknown): string {
  const text = String(value || "").trim();
  if (!text) return "";
  const match = text.match(/^([0-9]+(?:\.[0-9]+)?)(.*)$/);
  if (!match) return text.slice(0, 32);
  let number = match[1];
  let unit = match[2].trim();
  if (/^g+$/.test(unit)) unit = "g";
  for (const known of ["克", "个", "份", "盒", "瓶", "袋", "根", "把", "颗", "杯", "块"]) {
    if (unit.length > known.length && unit === known.repeat(unit.length / known.length)) {
      unit = known;
    }
  }
  if (/^[0-9]+$/.test(number)) {
    const maxDigits = ["g", "克", "ml", "毫升"].includes(unit) ? 3 : 2;
    const pollutedPrefix = number.length > maxDigits && number.startsWith("11");
    while (pollutedPrefix && number.length > maxDigits && number.startsWith("1")) {
      number = number.slice(1);
    }
    if (number.length > 1 && /^1+$/.test(number)) number = "1";
  }
  return `${number}${unit}`.slice(0, 32);
}

export function formatInventoryQuantity(item: Pick<InventoryItem, "quantity" | "unit">): string {
  const unit = String(item.unit || "").trim();
  if (/^[0-9]+(?:\.[0-9]+)?\s*[^\d\s]+/.test(unit)) {
    return cleanQuantityText(unit);
  }
  return cleanQuantityText(`${item.quantity || 0}${unit || "份"}`);
}
