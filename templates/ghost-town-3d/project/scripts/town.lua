-- Town: wires the main menu (ui/main_menu.ui.json). ui.* works on any
-- script: show / hide / set_text / set_value / set_visible / on_click /
-- on_change.
function on_start()
  ui.on_click("menu_ride", function()
    ui.hide("main_menu")
    toast("Welcome to Ghost Town", 3)
  end)
  ui.on_click("menu_credits", function()
    ui.set_visible("menu_note", true)
  end)
end
