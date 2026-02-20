library(ggplot2)
library(ggpattern)


draw_hex_logo_ggplot <- function() {
  
  # 1. Define Hexagon Coordinates
  # Angles for pointy top: 90, 30, -30, -90, -150, 150
  angles <- c(-150, -90, -30, 30, 90, 150) * (pi / 180)
  hex_data <- data.frame(
    x = 0.5 + 0.5 * cos(angles),
    y = 0.5 + 0.5 * sin(angles)
  )
  
  
  ggplot() +
    geom_polygon_pattern(
      data  = hex_data, 
      aes(x = x, y = y),
      fill  = "#F5F5F5",  # Light Gray
      color = "#4682B4",  # Steel Blue
      linewidth = 1.75,
      pattern          = 'image',
      pattern_filename = "pkgdown/fill.png",
      pattern_type     = 'expand',    # Key for avoiding stretch
      pattern_gravity  = 'center',    # Centers the image
      pattern_scale    = 1.2,         # Increase this until it overflows the bounds
      pattern_res      = 200,         # High res for the render
    ) +
    coord_fixed() +
    theme_void()
}

# Generate and print the plot
p <- draw_hex_logo_ggplot()
print(p)

# Save to file
ggsave("man/figures/logo.png", p, width = 200, height = 200, units = "px", bg = "transparent", dpi = 100)

