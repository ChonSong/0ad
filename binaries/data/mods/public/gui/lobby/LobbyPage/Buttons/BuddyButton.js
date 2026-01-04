/**
 * This class manages the button that enables the player to add or remove buddies.
 */
class BuddyButton
{
	constructor(xmppMessages)
	{
		this.buddyChangedHandlers = new Set();
		this.playerName = undefined;

		this.toggleBuddyButton = Engine.GetGUIObjectByName("toggleBuddyButton");
		this.toggleBuddyButton.onPress = this.onPress.bind(this);

		const rebuildAndNotify = this.rebuildAndNotify.bind(this);
		xmppMessages.registerXmppMessageHandler("system", "connected", rebuildAndNotify);
		xmppMessages.registerXmppMessageHandler("system", "disconnected", rebuildAndNotify);
		xmppMessages.registerXmppMessageHandler("roster", "change", rebuildAndNotify);

		this.rebuild();
	}

	registerBuddyChangeHandler(handler)
	{
		this.buddyChangedHandlers.add(handler);
	}

	onPlayerSelectionChange(playerName)
	{
		this.playerName = playerName;
		this.rebuild();
	}

	rebuild()
	{
		const isBuddy = !!this.playerName && Engine.IsLobbyBuddy(this.playerName);

		this.toggleBuddyButton.caption = isBuddy ? this.UnmarkString : this.MarkString;

		this.toggleBuddyButton.enabled = Engine.IsXmppClientConnected() && !!this.playerName && this.playerName != g_Nickname;
	}

	rebuildAndNotify()
	{
		this.rebuild();

		for (const handler of this.buddyChangedHandlers)
			handler();
	}

	/**
	 * Toggle the buddy state of the selected player.
	 */
	onPress()
	{
		if (!this.playerName || this.playerName == g_Nickname)
			return;

		if (Engine.IsLobbyBuddy(this.playerName))
			Engine.LobbyRemoveBuddy(this.playerName);
		else
			Engine.LobbyAddBuddy(this.playerName);
	}
}

BuddyButton.prototype.MarkString = translate("Mark as Buddy");
BuddyButton.prototype.UnmarkString = translate("Unmark as Buddy");
