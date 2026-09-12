local paths = {"/index", "/login", "/picture", "/register", "/video", "/index.html",
               "/picture.html", "/video.html", "/welcome.html",
               "/hello", "/status", "/ping"}

request = function()
    return wrk.format(nil, paths[math.random(#paths)])
end
