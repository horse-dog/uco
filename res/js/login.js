/**
 * @file login.js
 * @brief 登录页逻辑 (cookie + session + csrf).
 *
 * 流程:
 *   1. 页面加载即取 csrf token (服务端同时种下 session cookie);
 *   2. 前端预校验 + 防重复提交, fetch 提交 username/password
 *      (csrf_token 走 X-CSRF-Token 头);
 *   3. 按 JSON 响应联动 UI: 错误使用浮层提示 + 输入框变色 + 聚焦,
 *      输入即清错; 凭证错误清空密码框; 403 时刷新 token 供重试.
 */

const form = document.getElementById('login-form');
const usernameInput = document.getElementById('username');
const passwordInput = document.getElementById('password');
const noticeBox = document.getElementById('login-notice');
const noticeText = document.getElementById('login-notice-text');
const submitBtn = form.querySelector('button[type="submit"]');

let csrfToken = '';
let toastTimer = 0;
let toastCleanupTimer = 0;
let toastShowFrame = 0;

function hideToast() {
    clearTimeout(toastTimer);
    cancelAnimationFrame(toastShowFrame);
    noticeBox.classList.remove('is-visible');
    noticeBox.classList.add('is-leaving');
    clearTimeout(toastCleanupTimer);
    toastCleanupTimer = setTimeout(() => {
        noticeBox.classList.remove('is-leaving');
        noticeBox.setAttribute('aria-hidden', 'true');
    }, 700);
}

function showToast(text) {
    clearTimeout(toastTimer);
    clearTimeout(toastCleanupTimer);
    cancelAnimationFrame(toastShowFrame);
    noticeText.textContent = text;
    noticeBox.classList.remove('is-leaving');
    noticeBox.setAttribute('aria-hidden', 'false');
    toastShowFrame = requestAnimationFrame(() => {
        noticeBox.classList.add('is-visible');
    });
    toastTimer = setTimeout(hideToast, 3000);
}

/** 一次性跳转提示：未登录跳转由 cookie 传递，注册后自动登录失败由
 *  同标签页 sessionStorage 传递；读取后立即删除，刷新时不重复出现. */
(function showLoginNotice() {
    const cookies = Object.fromEntries(
        document.cookie.split('; ').map(c => c.split('=')));
    let notice = cookies.login_notice;
    if (notice) {
        document.cookie = 'login_notice=; max-age=0; path=/login';
    }

    try {
        const storedNotice = sessionStorage.getItem('login_notice');
        if (storedNotice) {
            notice = storedNotice;
            sessionStorage.removeItem('login_notice');
        }
    } catch (e) {
        // 存储不可用时仍可正常登录.
    }

    const text = {
        not_logged_in: '您还未登录，请先登录',
        registered: '注册成功，请登录'
    }[notice];
    if (text) {
        showToast(text);
    }
})();

/** 错误文案 (前端映射, 不依赖后端 msg, 便于统一中文/多语言). */
const errMsg = {
    fetchCsrf: '无法获取登录凭证, 请刷新重试',
    badCredentials: '用户名或密码错误',
    missingField: '请输入用户名和密码',
    csrfFailed: '页面已过期, 请重新提交',
    server: '服务异常, 请稍后重试',
    network: '网络错误, 请重试'
};

/** 展示错误: 统一浮层提示 + 输入框变色 + 聚焦 (凭证错时清空密码框). */
function showError(text, clearPassword) {
    showToast(text);
    usernameInput.classList.add('is-invalid');
    passwordInput.classList.add('is-invalid');
    if (clearPassword) {
        passwordInput.value = '';
        passwordInput.focus();
    } else {
        usernameInput.focus();
    }
}

/** 清除错误状态；仅改变颜色，不改变输入框边框宽度和布局. */
function clearError() {
    hideToast();
    usernameInput.classList.remove('is-invalid');
    passwordInput.classList.remove('is-invalid');
}

// 常规交互: 任一输入框有输入即清除上一次的错误提示.
form.addEventListener('input', clearError);

// 取 csrf token (服务端同时种下 session cookie).
fetch('/api/csrf')
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(d => { csrfToken = d.body.csrf_token; })
    .catch(() => showError(errMsg.fetchCsrf));

form.addEventListener('submit', async (ev) => {
    ev.preventDefault(); // 阻止原生同步提交, 改走 fetch.
    clearError();

    // 前端预校验: 空值不出门.
    if (!usernameInput.value.trim() || !passwordInput.value) {
        showError(errMsg.missingField);
        return;
    }

    // 防重复提交.
    submitBtn.disabled = true;
    submitBtn.textContent = '登录中...';
    try {
        const body = new URLSearchParams({
            username: usernameInput.value.trim(),
            password: passwordInput.value
        });
        const resp = await fetch('/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded',
                       'X-CSRF-Token': csrfToken },
            body: body
        });
        const data = await resp.json();

        if (resp.ok) {
            window.location.href = '/welcome';
            return;
        }
        if (resp.status === 401) { // 凭证错误: 清空密码, 聚焦重输.
            showError(errMsg.badCredentials, true);
        } else if (resp.status === 403) { // token 过期/失效: 刷新供重试.
            showError(errMsg.csrfFailed);
            const r = await fetch('/api/csrf');
            if (r.ok) csrfToken = (await r.json()).body.csrf_token;
        } else if (resp.status === 400) {
            showError(errMsg.missingField);
        } else {
            showError(data.msg || errMsg.server);
        }
    } catch (e) {
        showError(errMsg.network);
    } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = '确认';
    }
});
