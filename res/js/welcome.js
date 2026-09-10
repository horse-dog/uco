/**
 * @file welcome.js
 * @brief 欢迎页逻辑: 展示当前用户并退出登录.
 *
 * 页面由服务端 /welcome 路由保护；加载后从 /api/me 读取用户名。
 * 点击注销 → 取 csrf token → POST /logout → 跳转 /login.
 */

const logoutBtn = document.getElementById('logout-btn');
const usernameText = document.getElementById('welcome-username');
const noticeBox = document.getElementById('welcome-notice');
const noticeText = document.getElementById('welcome-notice-text');
let loggingOut = false;

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

/** 一次性跳转提示: 已登录访问登录/注册页被弹回时由 cookie 传递,
 * 读取后立即删除, 刷新时不重复出现. */
(function showWelcomeNotice() {
    const cookies = Object.fromEntries(
        document.cookie.split('; ').map(c => c.split('=')));
    const notice = cookies.welcome_notice;
    if (!notice) {
        return;
    }
    document.cookie = 'welcome_notice=; max-age=0; path=/welcome';
    if (notice === 'already_logged_in') {
        showToast('您已登录');
    }
})();

fetch('/api/me')
    .then(resp => {
        if (resp.status === 401) {
            window.location.href = '/login';
            return null;
        }
        return resp.ok ? resp.json() : Promise.reject(resp.status);
    })
    .then(data => {
        if (data) {
            usernameText.textContent = data.body.username;
        }
    })
    .catch(() => {
        usernameText.textContent = '';
    });

logoutBtn.addEventListener('click', async (ev) => {
    ev.preventDefault();
    if (loggingOut) {
        return;
    }

    loggingOut = true;
    logoutBtn.setAttribute('aria-disabled', 'true');
    try {
        // 取 csrf token (session 中的 token 登录时已刷新, 重新获取),
        // 经 X-CSRF-Token 头提交 (logout 无表单字段).
        const cr = await fetch('/api/csrf');
        const token = cr.ok ? (await cr.json()).body.csrf_token : '';

        const resp = await fetch('/logout', {
            method: 'POST',
            headers: { 'X-CSRF-Token': token }
        });
        if (resp.ok) {
            window.location.href = '/login';
            return;
        }
        throw new Error('logout failed');
    } catch (e) {
        loggingOut = false;
        logoutBtn.removeAttribute('aria-disabled');
        alert('注销失败，请重试');
    }
});
