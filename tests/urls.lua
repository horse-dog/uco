local paths = {"/index", "/login", "/picture", "/register", "/video", "/welcome", "/index.html", "/login.html",
               "/picture.html", "/register.html", "/video.html", "/welcome.html", "/aa.html", "/cc.html",
               "/redirect", "/status", "/head"}

request = function()
    return wrk.format(nil, paths[math.random(#paths)])
end
