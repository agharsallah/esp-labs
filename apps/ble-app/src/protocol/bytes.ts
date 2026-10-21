// Bytes as people read them: "05 00 F8 A0".
export const hex = (bytes: Uint8Array | ArrayLike<number>) =>
  Array.from(bytes, (b) => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

export const viewBytes = (v: DataView) => new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
