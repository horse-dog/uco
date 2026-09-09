/**
 * @file welcome.js
 * @brief 欢迎页逻辑: 展示当前用户并退出登录.
 *
 * 页面由服务端 /welcome 路由保护；加载后从 /api/me 读取用户名。
 * 点击注销 → 取 csrf token → POST /logout → 跳转 /login.
 */

const logoutBtn = document.getElementById('logout-btn');
const usernameText = document.getElementById('welcome-username');
let loggingOut = false;

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
            usernameText.textContent = data.username;
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
        // 取 csrf token (session 中的 token 登录时已刷新, 重新获取).
        const cr = await fetch('/api/csrf');
        const token = cr.ok ? (await cr.json()).csrf_token : '';

        const resp = await fetch('/logout', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: new URLSearchParams({ csrf_token: token })
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
