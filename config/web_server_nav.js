(() => {
  if (window.__hatiWebNavigationLoaded) return;
  window.__hatiWebNavigationLoaded = true;

  const navigation = document.createElement("nav");
  navigation.setAttribute("aria-label", "Device tools");
  Object.assign(navigation.style, {
    position: "fixed",
    top: "20px",
    right: "60px",
    zIndex: "2147483647",
    display: "flex",
    gap: "8px",
    padding: "8px",
    borderRadius: "10px",
    background: "rgba(30, 30, 30, 0.9)",
    boxShadow: "0 2px 10px rgba(0, 0, 0, 0.35)",
    font: "14px system-ui, sans-serif",
  });

  [
    ["HATiSetup", "/admin"],
    ["HATiConfig", "/admin/tiles"],
    ["Device Screenshot", "/screenshot.png?view=1"],
    ["SD card", "/sdcard/"],
    ["SPIFFS", "/spiffs/"],
  ].forEach(([label, path]) => {
    const link = document.createElement("a");
    link.textContent = label;
    link.href = path;
    Object.assign(link.style, {
      color: "#fff",
      padding: "7px 10px",
      borderRadius: "7px",
      background: "#03a9f4",
      textDecoration: "none",
      whiteSpace: "nowrap",
    });
    navigation.appendChild(link);
  });

  document.body.appendChild(navigation);

  const frontend = document.createElement("script");
  frontend.src = "https://oi.esphome.io/v2/www.js";
  frontend.onerror = () => {
    const message = document.createElement("p");
    message.textContent = "Unable to load the ESPHome web interface.";
    message.style.margin = "80px 16px 16px";
    document.body.appendChild(message);
  };
  document.head.appendChild(frontend);
})();
