const configFields = [
  "wifi_ssid",
  "wifi_password",
  "llm_api_key",
  "llm_backend_type",
  "llm_profile",
  "llm_model",
  "llm_base_url",
  "llm_auth_type",
  "llm_timeout_ms",
  "qq_app_id",
  "qq_app_secret",
  "tg_bot_token",
  "wechat_token",
  "wechat_base_url",
  "wechat_cdn_base_url",
  "wechat_account_id",
  "search_brave_key",
  "search_tavily_key",
  "lua_base_dir",
  "time_timezone",
  "tts_xfyun_app_id",
  "tts_xfyun_api_key",
  "tts_xfyun_api_secret",
  "tts_xfyun_voice_name",
  "tts_xfyun_audio_encoding",
  "tts_xfyun_audio_format",
  "tts_xfyun_text_encoding",
  "tts_xfyun_websocket_uri",
  "tts_xfyun_auth_host",
  "tts_timeout_ms",
];

let currentPath = "/";
let wechatLoginPollTimer = null;

function showBanner(id, message, isError = false) {
  const banner = document.getElementById(id);
  banner.textContent = message;
  banner.classList.remove("hidden", "error");
  if (isError) {
    banner.classList.add("error");
  }
}

function hideBanner(id) {
  const banner = document.getElementById(id);
  banner.classList.add("hidden");
  banner.classList.remove("error");
}

function readConfigForm() {
  const payload = {};
  configFields.forEach((field) => {
    const input = document.getElementById(field);
    payload[field] = input ? input.value.trim() : "";
  });
  return payload;
}

function fillConfigForm(data) {
  configFields.forEach((field) => {
    const input = document.getElementById(field);
    if (input && typeof data[field] === "string") {
      input.value = data[field];
    }
  });
}

function humanSize(value) {
  if (value < 1024) {
    return `${value} B`;
  }
  if (value < 1024 * 1024) {
    return `${(value / 1024).toFixed(1)} KB`;
  }
  return `${(value / (1024 * 1024)).toFixed(1)} MB`;
}

function parentPath(path) {
  if (path === "/") {
    return "/";
  }
  const parts = path.split("/").filter(Boolean);
  parts.pop();
  return parts.length ? `/${parts.join("/")}` : "/";
}

function joinPath(base, name) {
  return base === "/" ? `/${name}` : `${base}/${name}`;
}

async function loadStatus() {
  const response = await fetch("/api/status", { cache: "no-store" });
  const data = await response.json();
  document.getElementById("wifiStatus").textContent = data.wifi_connected ? "Wi-Fi connected" : "Wi-Fi offline";
  document.getElementById("ipAddress").textContent = `IP: ${data.ip || "-"}`;
  document.getElementById("storagePath").textContent = `Storage: ${data.storage_base_path || "-"}`;
}

async function loadConfig() {
  hideBanner("configMessage");
  const response = await fetch("/api/config", { cache: "no-store" });
  if (!response.ok) {
    throw new Error("Failed to load settings");
  }
  fillConfigForm(await response.json());
}

async function saveConfig() {
  const button = document.getElementById("saveConfigButton");
  button.disabled = true;
  hideBanner("configMessage");

  try {
    const response = await fetch("/api/config", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(readConfigForm()),
    });
    const result = await response.json();
    if (!response.ok) {
      throw new Error(result.error || "Failed to save settings");
    }
    showBanner("configMessage", result.message || "Settings saved");
  } catch (error) {
    showBanner("configMessage", error.message, true);
  } finally {
    button.disabled = false;
  }
}

function stopWechatLoginPolling() {
  if (wechatLoginPollTimer) {
    clearTimeout(wechatLoginPollTimer);
    wechatLoginPollTimer = null;
  }
}

function renderWechatLoginStatus(data) {
  const qrImage = document.getElementById("wechatLoginQr");
  const qrLink = document.getElementById("wechatLoginQrLink");
  const meta = document.getElementById("wechatLoginMeta");

  if (data.qr_data_url) {
    qrImage.src = `https://api.qrserver.com/v1/create-qr-code/?size=320x320&data=${encodeURIComponent(data.qr_data_url)}`;
    qrImage.classList.remove("hidden");
    qrLink.href = data.qr_data_url;
    qrLink.textContent = data.qr_data_url;
    qrLink.classList.remove("hidden");
  } else {
    qrImage.removeAttribute("src");
    qrImage.classList.add("hidden");
    qrLink.removeAttribute("href");
    qrLink.textContent = "";
    qrLink.classList.add("hidden");
  }

  meta.textContent = data.status ? `Status: ${data.status}` : "Status: idle";
  if (data.message) {
    showBanner("wechatLoginMessage", data.message, false);
  } else {
    hideBanner("wechatLoginMessage");
  }

  if (data.completed && data.persisted) {
    document.getElementById("wechat_token").value = "";
    if (typeof data.base_url === "string" && data.base_url) {
      document.getElementById("wechat_base_url").value = data.base_url;
    }
    if (typeof data.account_id === "string" && data.account_id) {
      document.getElementById("wechat_account_id").value = data.account_id;
    }
  }
}

async function pollWechatLoginStatus() {
  try {
    const response = await fetch("/api/wechat/login/status", { cache: "no-store" });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || "Failed to fetch WeChat login status");
    }
    renderWechatLoginStatus(data);
    if (data.active || (data.completed && !data.persisted)) {
      wechatLoginPollTimer = setTimeout(pollWechatLoginStatus, 1500);
    } else {
      stopWechatLoginPolling();
    }
  } catch (error) {
    showBanner("wechatLoginMessage", error.message || "Failed to poll WeChat login status", true);
    stopWechatLoginPolling();
  }
}

async function startWechatLogin() {
  const button = document.getElementById("wechatLoginStartButton");
  button.disabled = true;
  hideBanner("wechatLoginMessage");

  try {
    const response = await fetch("/api/wechat/login/start", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        account_id: document.getElementById("wechat_account_id").value.trim(),
        force: true,
      }),
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || "Failed to start WeChat login");
    }
    renderWechatLoginStatus(data);
    stopWechatLoginPolling();
    wechatLoginPollTimer = setTimeout(pollWechatLoginStatus, 1000);
  } catch (error) {
    showBanner("wechatLoginMessage", error.message || "Failed to start WeChat login", true);
  } finally {
    button.disabled = false;
  }
}

async function cancelWechatLogin() {
  try {
    const response = await fetch("/api/wechat/login/cancel", {
      method: "POST",
      cache: "no-store",
    });
    const data = await response.json();
    if (!response.ok) {
      throw new Error(data.error || "Failed to cancel WeChat login");
    }
    renderWechatLoginStatus({
      status: "cancelled",
      message: data.message || "已取消微信登录。",
      qr_data_url: "",
      completed: false,
      persisted: false,
    });
    stopWechatLoginPolling();
  } catch (error) {
    showBanner("wechatLoginMessage", error.message || "Failed to cancel WeChat login", true);
  }
}

function renderFileRows(entries) {
  const tbody = document.getElementById("fileTableBody");
  tbody.innerHTML = "";

  if (!entries.length) {
    const row = document.createElement("tr");
    row.innerHTML = "<td colspan=\"4\">This folder is empty.</td>";
    tbody.appendChild(row);
    return;
  }

  entries
    .sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name))
    .forEach((entry) => {
      const row = document.createElement("tr");
      const typeLabel = entry.is_dir ? "Folder" : "File";
      const sizeLabel = entry.is_dir ? "-" : humanSize(entry.size || 0);
      row.innerHTML = `
        <td>${entry.name}</td>
        <td>${typeLabel}</td>
        <td>${sizeLabel}</td>
        <td class="actions"></td>
      `;

      const actions = row.querySelector(".actions");

      if (entry.is_dir) {
        const openButton = document.createElement("button");
        openButton.className = "link-button";
        openButton.textContent = "Open";
        openButton.onclick = () => {
          currentPath = entry.path;
          loadFiles().catch((error) => showBanner("fileMessage", error.message, true));
        };
        actions.appendChild(openButton);
      } else {
        const download = document.createElement("a");
        download.href = `/files${entry.path}`;
        download.textContent = "Download";
        download.className = "link-button";
        download.target = "_blank";
        actions.appendChild(download);
      }

      const deleteButton = document.createElement("button");
      deleteButton.className = "link-button";
      deleteButton.textContent = "Delete";
      deleteButton.onclick = async () => {
        if (!window.confirm(`Delete ${entry.path}?`)) {
          return;
        }
        await deletePath(entry.path);
      };
      actions.appendChild(deleteButton);

      tbody.appendChild(row);
    });
}

async function loadFiles() {
  hideBanner("fileMessage");
  document.getElementById("currentPath").textContent = currentPath;

  const response = await fetch(`/api/files?path=${encodeURIComponent(currentPath)}`, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(await response.text() || "Failed to load file list");
  }

  const data = await response.json();
  currentPath = data.path || "/";
  document.getElementById("currentPath").textContent = currentPath;
  renderFileRows(data.entries || []);
}

async function uploadFile() {
  const pathInput = document.getElementById("uploadPathInput");
  const fileInput = document.getElementById("uploadFileInput");
  const button = document.getElementById("uploadButton");
  const file = fileInput.files[0];
  const relativePath = pathInput.value.trim() || (file ? joinPath(currentPath, file.name) : "");

  if (!file || !relativePath.startsWith("/")) {
    showBanner("fileMessage", "Select a file and provide a target path that starts with /.", true);
    return;
  }

  button.disabled = true;
  hideBanner("fileMessage");
  try {
    const response = await fetch(`/api/files/upload?path=${encodeURIComponent(relativePath)}`, {
      method: "POST",
      cache: "no-store",
      body: file,
    });
    if (!response.ok) {
      throw new Error(await response.text());
    }
    pathInput.value = "";
    fileInput.value = "";
    showBanner("fileMessage", "Upload completed");
    await loadFiles();
  } catch (error) {
    showBanner("fileMessage", error.message || "Upload failed", true);
  } finally {
    button.disabled = false;
  }
}

async function createFolder() {
  const input = document.getElementById("newFolderInput");
  const name = input.value.trim();
  if (!name) {
    showBanner("fileMessage", "Enter a folder name.", true);
    return;
  }

  try {
    const response = await fetch("/api/files/mkdir", {
      method: "POST",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: joinPath(currentPath, name) }),
    });
    if (!response.ok) {
      throw new Error(await response.text());
    }
    input.value = "";
    showBanner("fileMessage", "Folder created");
    await loadFiles();
  } catch (error) {
    showBanner("fileMessage", error.message || "Failed to create folder", true);
  }
}

async function deletePath(path) {
  try {
    const response = await fetch(`/api/files?path=${encodeURIComponent(path)}`, {
      method: "DELETE",
      cache: "no-store",
    });
    if (!response.ok) {
      throw new Error(await response.text());
    }
    showBanner("fileMessage", "Delete completed");
    await loadFiles();
  } catch (error) {
    showBanner("fileMessage", error.message || "Delete failed", true);
  }
}

function bindEvents() {
  document.getElementById("saveConfigButton").addEventListener("click", saveConfig);
  document.getElementById("wechatLoginStartButton").addEventListener("click", startWechatLogin);
  document.getElementById("wechatLoginCancelButton").addEventListener("click", cancelWechatLogin);
  document.getElementById("refreshFilesButton").addEventListener("click", () => loadFiles().catch((error) => {
    showBanner("fileMessage", error.message, true);
  }));
  document.getElementById("upDirButton").addEventListener("click", () => {
    currentPath = parentPath(currentPath);
    loadFiles().catch((error) => showBanner("fileMessage", error.message, true));
  });
  document.getElementById("uploadButton").addEventListener("click", uploadFile);
  document.getElementById("chooseFileButton").addEventListener("click", () => {
    document.getElementById("uploadFileInput").click();
  });
  document.getElementById("createFolderButton").addEventListener("click", createFolder);
  document.getElementById("uploadFileInput").addEventListener("change", (event) => {
    const file = event.target.files[0];
    const selectedFileName = document.getElementById("selectedFileName");
    if (file) {
      document.getElementById("uploadPathInput").value = joinPath(currentPath, file.name);
      selectedFileName.textContent = file.name;
    } else {
      selectedFileName.textContent = "No file selected";
    }
  });
}

async function bootstrap() {
  bindEvents();
  try {
    await loadStatus();
  } catch (error) {
    showBanner("configMessage", error.message || "Failed to load device status", true);
  }

  try {
    await loadConfig();
  } catch (error) {
    showBanner("configMessage", error.message || "Failed to load settings", true);
  }

  try {
    await pollWechatLoginStatus();
  } catch (error) {
    showBanner("wechatLoginMessage", error.message || "Failed to load WeChat login status", true);
  }

  try {
    await loadFiles();
  } catch (error) {
    showBanner("fileMessage", error.message || "Failed to load file list", true);
  }
}

bootstrap().catch((error) => {
  showBanner("configMessage", error.message || "Failed to initialize the page", true);
});
