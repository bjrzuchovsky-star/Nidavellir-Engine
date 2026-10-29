// A small 2D example. Worth shows in the Inspector. The trigger pockets the coin.
using TombStone;
[Script("coin")]
public class Coin : Script
{
    public override void OnStart()
    {
        Log("coin worth " + Prop("worth", 1));
    }
    public override void OnTriggerEnter(ulong other)
    {
        Toast("Coin", "Pocketed");
        SetVisible(Self, false);
    }
}
