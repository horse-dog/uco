package main

import "github.com/gin-gonic/gin"

func main() {
	r := gin.New() // ← 改这里：不自带 Logger/Recovery
	r.GET("/hello", func(c *gin.Context) {
		c.Header("Content-Type", "text/html; charset=utf-8")
		c.String(200, "<h1>Hello World!</h1>")
	})
	r.GET("/index", func(c *gin.Context) {
		c.File("../../res/index.html")
	})
	r.Run(":8080")
}
