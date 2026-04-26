# 安装 + 配置 + 启动，复制粘贴一次性执行：
sudo apt install -y nginx && \
echo '<h1>Hello World!</h1>' | sudo tee /usr/share/nginx/html/hello.html >/dev/null && \
cat <<'CONF' | sudo tee /etc/nginx/sites-available/hello_server >/dev/null
server {
    listen 8081;
    server_name 127.0.0.1;

    location = /hello {
        default_type text/html;
        return 200 "<h1>Hello World!</h1>";
    }
}
CONF
sudo ln -sf /etc/nginx/sites-available/hello_server /etc/nginx/sites-enabled/ && \
sudo rm -f /etc/nginx/sites-enabled/default && \
sudo nginx -t && sudo systemctl restart nginx && \
curl -v http://127.0.0.1:8081/hello
