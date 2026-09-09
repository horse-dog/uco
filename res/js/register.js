/**
 * @file register.js
 * @brief 注册页逻辑 (cookie + session + csrf).
 *
 * 流程:
 *   1. 页面加载即取 csrf token (服务端同时种下 session cookie);
 *   2. 前端预校验 (非空/长度/两次密码一致);
 *   3. fetch 提交 username/password/csrf_token;
 *   4. 成功后跳转登录页; 403 时刷新 token 供重试.
 */

const form = document.getElementById('register-form');
const usernameInput = document.getElementById('username');
const passwordInput = document.getElementById('password');
const confirmInput = document.getElementById('confirm');
const noticeBox = document.getElementById('register-notice');
const noticeText = document.getElementById('register-notice-text');
const submitBtn = form.querySelector('button[type="submit"]');

let csrfToken = '';
let toastTimer = 0;
let toastCleanupTimer = 0;
let toastShowFrame = 0;

/** 错误文案 (前端映射; 规则契约见后端 user.proto RegisterReq 注释). */
const errMsg = {
    fetchCsrf: '无法获取注册凭证, 请刷新重试',
    missingField: '请完整填写所有字段',
    usernameLen: '用户名须为 3-64 字符, 仅限字母/数字/下划线/横线',
    passwordLen: '密码须为 8-64 字节',
    confirmMismatch: '两次输入的密码不一致',
    usernameTaken: '用户名已被占用',
    badParams: '用户名或密码格式不符合要求',
    csrfFailed: '页面已过期, 请重新提交',
    server: '服务异常, 请稍后重试',
    network: '网络错误, 请重试'
};

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

function showError(text, focusInput) {
    showToast(text);
    [usernameInput, passwordInput, confirmInput]
        .forEach(el => el.classList.add('is-invalid'));
    (focusInput || usernameInput).focus();
}

function clearError() {
    hideToast();
    [usernameInput, passwordInput, confirmInput]
        .forEach(el => el.classList.remove('is-invalid'));
}

form.addEventListener('input', clearError);

fetch('/api/csrf')
    .then(r => r.ok ? r.json() : Promise.reject(r.status))
    .then(d => { csrfToken = d.csrf_token; })
    .catch(() => showError(errMsg.fetchCsrf));

form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    clearError();

    // 前端预校验.
    if (!usernameInput.value.trim() || !passwordInput.value ||
        !confirmInput.value) {
        showError(errMsg.missingField);
        return;
    }
    const username = usernameInput.value.trim();
    const password = passwordInput.value;
    // 用户名白名单 [A-Za-z0-9_-] (规则契约见后端 user.proto RegisterReq 注释).
    if (!/^[A-Za-z0-9_-]{3,64}$/.test(username)) {
        showError(errMsg.usernameLen);
        return;
    }
    if (password.length < 8 || password.length > 64) {
        showError(errMsg.passwordLen, passwordInput);
        return;
    }
    if (password !== confirmInput.value) {
        showError(errMsg.confirmMismatch, confirmInput);
        return;
    }

    submitBtn.disabled = true;
    submitBtn.textContent = '注册中...';
    try {
        const body = new URLSearchParams({
            username: username,
            password: password,
            csrf_token: csrfToken
        });
        const resp = await fetch('/register', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: body
        });
        const data = await resp.json();

        if (resp.ok) {
            // 同标签页携带一次性成功提示，不依赖已销毁的匿名 session.
            try {
                sessionStorage.setItem('login_notice', 'registered');
            } catch (e) {
                // 存储不可用不影响主流程.
            }
            window.location.href = '/login';
            return;
        }
        if (resp.status === 409) {
            showError(errMsg.usernameTaken);
        } else if (resp.status === 403) {
            showError(errMsg.csrfFailed);
            const r = await fetch('/api/csrf');
            if (r.ok) csrfToken = (await r.json()).csrf_token;
        } else if (resp.status === 400) {
            // 400 含"密码过于简单"(后端黑名单, 前端无法预判), 透传后端 msg.
            showError(data.msg || errMsg.badParams);
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
