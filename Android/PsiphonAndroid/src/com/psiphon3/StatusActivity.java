/*
 * Copyright (c) 2012, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

package com.psiphon3;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityManager.RunningServiceInfo;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.Bundle;
import android.support.v4.content.LocalBroadcastManager;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.ImageView;
import android.widget.ScrollView;
import android.widget.TableLayout;
import android.widget.TextView;

import com.psiphon3.PsiphonData.StatusMessage;
import com.psiphon3.UpgradeManager;
import com.psiphon3.UpgradeManager.UpgradeInstaller;
import com.psiphon3.Utils.MyLog;


public class StatusActivity extends Activity implements MyLog.ILogInfoProvider
{
    public static final String ADD_MESSAGE = "com.psiphon3.PsiphonAndroidActivity.ADD_MESSAGE";
    public static final String ADD_MESSAGE_TEXT = "com.psiphon3.PsiphonAndroidActivity.ADD_MESSAGE_TEXT";
    public static final String ADD_MESSAGE_CLASS = "com.psiphon3.PsiphonAndroidActivity.ADD_MESSAGE_CLASS";
    public static final String HANDSHAKE_SUCCESS = "com.psiphon3.PsiphonAndroidActivity.HANDSHAKE_SUCCESS";
    public static final String UNEXPECTED_DISCONNECT = "com.psiphon3.PsiphonAndroidActivity.UNEXPECTED_DISCONNECT";
    
    private TableLayout m_messagesTableLayout;
    private ScrollView m_messagesScrollView;
    private LocalBroadcastManager m_localBroadcastManager;
    
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        
        MyLog.logInfoProvider = this;

        setContentView(R.layout.main);
        
        m_messagesTableLayout = (TableLayout)findViewById(R.id.messagesTableLayout);
        m_messagesScrollView = (ScrollView)findViewById(R.id.messagesScrollView);

        // Note that this must come after the above lines, or else the activity
        // will not be sufficiently initialized for isDebugMode to succeed. (Voodoo.)
        PsiphonConstants.DEBUG = Utils.isDebugMode(this);

        // Restore messages previously posted by the service
        m_messagesTableLayout.removeAllViews();
        for (StatusMessage msg : PsiphonData.getPsiphonData().getStatusMessages())
        {
            addMessage(msg.m_message, msg.m_messageClass);
        }
        
        // Listen for new messages
        // Using local broad cast (http://developer.android.com/reference/android/support/v4/content/LocalBroadcastManager.html)
        
        m_localBroadcastManager = LocalBroadcastManager.getInstance(StatusActivity.this);

        m_localBroadcastManager.registerReceiver(
                new AddMessageReceiver(),
                new IntentFilter(ADD_MESSAGE));
    }
    
    @Override
    protected void onResume()
    {
        super.onResume();
        
        PsiphonData.getPsiphonData().setStatusActivityForeground(true);
        
        final Context context = this;

        UpgradeInstaller.IUpgradeListener upgradeListener = new UpgradeInstaller.IUpgradeListener()
        {
            @Override public void upgradeStarted()
            {
                // If an upgrade has been started, don't do anything else.
                return;
            }
            
            @Override public void upgradeNotStarted()
            {
                // Start tunnel service (if not already running)
                startService(new Intent(context, TunnelService.class));

                // Handle the intent that resumed that activity
                HandleCurrentIntent();
            }
        };
        
        if (!isServiceRunning())
        {
            UpgradeInstaller upgrader = new UpgradeManager.UpgradeInstaller(this);
            upgrader.doUpgrade(upgradeListener);
        }
        else
        {
            upgradeListener.upgradeNotStarted();
        }
    }
    
    @Override
    protected void onPause()
    {
        super.onResume();
        
        PsiphonData.getPsiphonData().setStatusActivityForeground(false);
    }

    @Override
    protected void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
            
        // If the app is already foreground (so onNewIntent is being called), 
        // the incoming intent is not automatically set as the activity's intent
        // (i.e., the intent returned by getIntent()). We want this behaviour, 
        // so we'll set it explicitly. 
        setIntent(intent);

        // Handle explicit intent that is received when activity is already running
        HandleCurrentIntent();
    }

    protected void HandleCurrentIntent()
    {
        Intent intent = getIntent();
        
        if (intent == null || intent.getAction() == null)
        {
            return;
        }

        if (0 == intent.getAction().compareTo(HANDSHAKE_SUCCESS))
        {
            Events.displayBrowser(this);
            
            // We only want to respond to the HANDSHAKE_SUCCESS action once,
            // so we need to clear it (by setting it to a non-special intent).
            setIntent(new Intent(
                            "ACTION_VIEW",
                            null,
                            this,
                            this.getClass()));
        }
        
        // No explicit action for UNEXPECTED_DISCONNECT, just show the activity
    }
    
    public void onOpenBrowserClick(View v)
    {
        Events.displayBrowser(this);       
    }
    
    public void onFeedbackClick(View v)
    {
        Intent feedbackIntent = new Intent(this, FeedbackActivity.class);
        startActivity(feedbackIntent);
    }

    public void onAboutClick(View v)
    {
        // TODO: if connected, open in Psiphon browser? 
        // Events.displayBrowser(this, Uri.parse(PsiphonConstants.INFO_LINK_URL));

        Intent browserIntent = new Intent(Intent.ACTION_VIEW, Uri.parse(EmbeddedValues.INFO_LINK_URL));
        startActivity(browserIntent);
    }
    
    public void onExitClick(View v)
    {
        // This command doesn't necessarily kill the process, but
        // it stops the service and hides the app.
        
        stopService(new Intent(this, TunnelService.class));
        this.moveTaskToBack(true);
    }
    
    public class AddMessageReceiver extends BroadcastReceiver
    {
        @Override
        public void onReceive(Context context, Intent intent)
        {
            String message = intent.getStringExtra(ADD_MESSAGE_TEXT);
            int messageClass = intent.getIntExtra(ADD_MESSAGE_CLASS, MESSAGE_CLASS_INFO);
            addMessage(message, messageClass);
        }
    }
    
    public static final int MESSAGE_CLASS_INFO = 0;
    public static final int MESSAGE_CLASS_ERROR = 1;
    public static final int MESSAGE_CLASS_DEBUG = 2;
    public static final int MESSAGE_CLASS_WARNING = 3;
    
    public void addMessage(String message, int messageClass)
    {
        int messageClassImageRes = 0;
        int messageClassImageDesc = 0;
        switch (messageClass)
        {
        case MESSAGE_CLASS_INFO:
            messageClassImageRes = android.R.drawable.presence_online;
            messageClassImageDesc = R.string.message_image_success_desc;
            break;
        case MESSAGE_CLASS_ERROR:
            messageClassImageRes = android.R.drawable.presence_busy;
            messageClassImageDesc = R.string.message_image_error_desc;
            break;
        case MESSAGE_CLASS_DEBUG:
            messageClassImageRes = android.R.drawable.presence_offline;
            messageClassImageDesc = R.string.message_image_debug_desc;
            break;
        case MESSAGE_CLASS_WARNING:
            messageClassImageRes = android.R.drawable.presence_invisible;
            messageClassImageDesc = R.string.message_image_warning_desc;
            break;
        }
        
        // 
        // Get the message row template and fill it in
        // 
        
        LayoutInflater inflater = (LayoutInflater) this.getSystemService(Context.LAYOUT_INFLATER_SERVICE);
        View rowView = inflater.inflate(R.layout.message_row, null);
        
        TextView textView = (TextView)rowView.findViewById(R.id.MessageRow_Text);
        textView.setText(message);
        
        ImageView imageView = (ImageView)rowView.findViewById(R.id.MessageRow_Image);
        imageView.setImageResource(messageClassImageRes);
        imageView.setContentDescription(getResources().getText(messageClassImageDesc));
        
        m_messagesTableLayout.addView(rowView);
        
        // Wait until the messages list is updated before attempting to scroll 
        // to the bottom.
        m_messagesScrollView.post(
            new Runnable()
            {
                @Override
                public void run()
                {
                    m_messagesScrollView.fullScroll(View.FOCUS_DOWN);
                }
            });
    }
    
    /**
     * Determine if the Psiphon local service is currently running.
     * @see <a href="http://stackoverflow.com/a/5921190/729729">From StackOverflow answer: "android: check if a service is running"</a>
     * @return True if the service is already running, false otherwise.
     */
    private boolean isServiceRunning() 
    {
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        for (RunningServiceInfo service : manager.getRunningServices(Integer.MAX_VALUE))
        {
            if (TunnelService.class.getName().equals(service.service.getClassName()))
            {
                return true;
            }
        }
        return false;
    }

    /**
     * Utils.MyLog.ILogInfoProvider implementation
     * For Android priority values, see <a href="http://developer.android.com/reference/android/util/Log.html">http://developer.android.com/reference/android/util/Log.html</a>
     */
    @Override
    public int getAndroidLogPriorityEquivalent(int priority)
    {
        switch (priority)
        {
        case Log.ERROR:
            return StatusActivity.MESSAGE_CLASS_ERROR;
        case Log.INFO:
            return StatusActivity.MESSAGE_CLASS_INFO;
        case Log.DEBUG:
            return StatusActivity.MESSAGE_CLASS_DEBUG;
        default:
            return StatusActivity.MESSAGE_CLASS_WARNING;
        }
    }

    @Override
    public String getResString(int stringResID, Object... formatArgs)
    {
        if (formatArgs == null || formatArgs.length == 0)
        {
            return getString(stringResID);
        }
        
        return getString(stringResID, formatArgs);
    }
}
