/**
 * @file remote-connection.cpp
 * @author Sina Karvandi (sina@hyperdbg.org)
 * @brief handle remote connections command
 * @details
 * @version 0.1
 * @date 2020-08-21
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#include "pch.h"

//
// Global Variables
//
extern BYTE g_EndOfBufferCheckTcp[TCP_END_OF_BUFFER_CHARS_COUNT];

extern BOOLEAN g_IsConnectedToHyperDbgLocally;
extern BOOLEAN g_IsConnectedToRemoteDebuggee;
extern BOOLEAN g_IsConnectedToRemoteDebugger;
extern BOOLEAN g_BreakPrintingOutput;
extern BOOLEAN g_IsEndOfMessageReceived;

extern SOCKET g_SeverSocket;
extern SOCKET g_ServerListenSocket;
extern SOCKET g_ClientConnectSocket;

extern HANDLE g_RemoteDebuggeeListeningThread;
extern HANDLE g_EndOfMessageReceivedEvent;

/**
 * @brief Listen of a port and wait for a client connection
 * @details this routine is supposed to be called by .listen command
 *
 * @param Port
 * @return VOID
 */
VOID
RemoteConnectionListen(PCSTR Port)
{
    char recvbuf[COMMUNICATION_BUFFER_SIZE] = {0};

    //
    // Check if the debugger or debuggee is already active
    //
    if (IsConnectedToAnyInstanceOfDebuggerOrDebuggee())
    {
        return;
    }

    //
    // Start server and wait for client
    //
    CommunicationServerCreateServerAndWaitForClient(Port, &g_SeverSocket, &g_ServerListenSocket);

    //
    // Check the version of debuggee and debugger
    //
    if (CommunicationServerReceiveMessage(g_SeverSocket, recvbuf, COMMUNICATION_BUFFER_SIZE) != 0)
    {
        //
        // Failed
        //
        ShowMessages("err, unable to handshake with the remote debugger\n");

        //
        // Close the connection
        //
        CommunicationServerShutdownAndCleanupConnection(g_SeverSocket, g_ServerListenSocket);

        return;
    }

    //
    // Check whether the signature of debuggee and debugger match or not
    //
    if (strcmp((const char *)BuildSignature, recvbuf) != 0)
    {
        //
        // Build version not matched
        //
        ShowMessages(ASSERT_MESSAGE_BUILD_SIGNATURE_DOESNT_MATCH);

        //
        // Send unsuccessful handshake (version match) message
        //
        if (CommunicationServerSendMessage(g_SeverSocket, "NO", 3) != 0)
        {
            //
            // Failed
            //
            ShowMessages("err, unable to handshake with the remote debugger\n");
        }

        //
        // Close the connection
        //
        CommunicationServerShutdownAndCleanupConnection(g_SeverSocket, g_ServerListenSocket);

        return;
    }
    else
    {
        //
        // Send successful handshake (version match) message
        //
        if (CommunicationServerSendMessage(g_SeverSocket, "OK", 3) != 0)
        {
            //
            // Failed
            //
            ShowMessages("err, unable to handshake with the remote debugger\n");

            //
            // Close the connection
            //
            CommunicationServerShutdownAndCleanupConnection(g_SeverSocket, g_ServerListenSocket);

            return;
        }
    }

    //
    // Indicate that it's a remote debugger
    //
    g_IsConnectedToRemoteDebugger = TRUE;

    //
    // And also, make it a local debugger
    //
    g_IsConnectedToHyperDbgLocally = TRUE;

    //
    // Zero the buffer for next command
    //
    RtlZeroMemory(recvbuf, COMMUNICATION_BUFFER_SIZE);

    while (true)
    {
        //
        // Receive message (this loop works as a command executer,
        // we don't send the results to the remote machine by using
        // this tools
        //
        if (CommunicationServerReceiveMessage(g_SeverSocket, recvbuf, COMMUNICATION_BUFFER_SIZE) != 0)
        {
            //
            // Failed, break
            //
            break;
        }

        //
        // Execute the command
        //
        int CommandExecutionResult = HyperDbgInterpreter(recvbuf);

        //
        // Send end of buffer
        //
        RemoteConnectionSendResultsToHost((const char *)g_EndOfBufferCheckTcp, sizeof(g_EndOfBufferCheckTcp));

        //
        // if the debugger encounters an exit state then the return will be 1
        //
        if (CommandExecutionResult == 1)
        {
            //
            // Exit from the debugger
            //
            exit(0);
        }

        //
        // Zero the buffer for next command
        //
        RtlZeroMemory(recvbuf, COMMUNICATION_BUFFER_SIZE);
    }

    //
    // Indicate that debugger is not connected
    //
    g_IsConnectedToHyperDbgLocally = FALSE;

    //
    // Indicate that it's note a remote debugger
    //
    g_IsConnectedToRemoteDebugger = FALSE;

    //
    // Indicate that we're not in remote debugger anymore
    //
    ShowMessages("closing the conntection...\n");

    //
    // Close the connection
    //
    CommunicationServerShutdownAndCleanupConnection(g_SeverSocket,
                                                    g_ServerListenSocket);
}

/**
 * @brief A thread that listens for server (debuggee) messages
 * and show it by using ShowMessages wrapper
 *
 * @param lpParam
 * @return DWORD
 */
DWORD WINAPI
RemoteConnectionThreadListeningToDebuggee(LPVOID lpParam)
{
    char   RecvBuf[COMMUNICATION_BUFFER_SIZE + TCP_END_OF_BUFFER_CHARS_COUNT] = {0};
    UINT32 BuffLenReceived                                                    = 0;

    while (g_IsConnectedToRemoteDebuggee)
    {
        printf("[ %s ]\n", __FUNCTION__);

        //
        // Receive message
        //
        if (CommunicationClientReceiveMessage(g_ClientConnectSocket, RecvBuf, COMMUNICATION_BUFFER_SIZE, &BuffLenReceived) != 0)
        {
            //
            // Failed, break
            //
            break;
        }

        //
        // Check if it's end of the buffer
        //
        for (size_t i = 0; i < BuffLenReceived; i++)
        {
            if (RecvBuf[i] == g_EndOfBufferCheckTcp[0] &&
                RecvBuf[i + 1] == g_EndOfBufferCheckTcp[1] &&
                RecvBuf[i + 2] == g_EndOfBufferCheckTcp[2] &&
                RecvBuf[i + 3] == g_EndOfBufferCheckTcp[3])
            {
                g_IsEndOfMessageReceived = TRUE;

                //
                // Cut the last string using \x00 \x00
                //
                RecvBuf[i]     = '\x00';
                RecvBuf[i + 1] = '\x00';
                break;
            }
        }

        //
        // This is just because we want to show a correct signature
        //
        if (!g_BreakPrintingOutput)
        {
            //
            // Show message from remote debuggee
            //
            ShowMessages("%s", RecvBuf);
        }

        //
        // Show the signature
        //
        if (g_IsEndOfMessageReceived)
        {
            //
            // it's not end of message anymore
            //
            g_IsEndOfMessageReceived = FALSE;

            //
            // Trigger the event
            //
            SetEvent(g_EndOfMessageReceivedEvent);
        }

        //
        // Clear the buffer
        //
        RtlZeroMemory(RecvBuf, COMMUNICATION_BUFFER_SIZE);
    }

    //
    // The connection was aborted
    // Uinitialize every connections
    //

    //
    // Indicate that debugger is not connected
    //
    g_IsConnectedToHyperDbgLocally = FALSE;

    //
    // Indicate that it's a remote debuggee
    //
    g_IsConnectedToRemoteDebuggee = FALSE;

    //
    // Show the signature
    //
    HyperDbgShowSignature();

    return 0;
}

/**
 * @brief Connect to a remote debuggee (guest) as a client (host)
 * @details this routine is supposed to be called by .connect command
 *
 * @param Ip
 * @param Port
 * @return VOID
 */
VOID
RemoteConnectionConnect(PCSTR Ip, PCSTR Port)
{
    DWORD  ThreadId;
    CHAR   Recv[3]  = {0};
    UINT32 BuffRecv = 0;

    //
    // Check if the debugger or debuggee is already active
    //
    if (IsConnectedToAnyInstanceOfDebuggerOrDebuggee())
    {
        return;
    }

    //
    // Connect to server
    //
    if (CommunicationClientConnectToServer(Ip, Port, &g_ClientConnectSocket) == 1)
    {
        //
        // There was an error
        //

        //
        // Indicate that debugger is not connected
        //
        g_IsConnectedToHyperDbgLocally = FALSE;

        //
        // Indicate that it's not a remote  debuggee
        //
        g_IsConnectedToRemoteDebuggee = FALSE;

        //
        // Shutdown connection
        //
        CommunicationClientShutdownConnection(g_ClientConnectSocket);

        //
        // Cleanup
        //
        CommunicationClientCleanup(g_ClientConnectSocket);
    }
    else
    {
        //
        // Connection was successful
        //

        //
        // Check to see whether the version of debugger and debuggee matches together or not
        //
        if (CommunicationClientSendMessage(g_ClientConnectSocket, (const char *)BuildSignature, sizeof(BuildSignature)) != 0)
        {
            //
            // Failed
            //
            ShowMessages("err, failed to communicate with debuggee\n");
            return;
        }

        //
        // Receive the handshake results
        //
        if (CommunicationClientReceiveMessage(g_ClientConnectSocket, Recv, sizeof(Recv), &BuffRecv) != 0)
        {
            //
            // Failed, break
            //
            ShowMessages("err, failed to receive message from debuggee\n");
            return;
        }

        //
        // Check if the handshake was successful or not
        //
        if (strcmp((const char *)"OK", Recv) != 0)
        {
            //
            // Build version not matched
            //
            ShowMessages(ASSERT_MESSAGE_BUILD_SIGNATURE_DOESNT_MATCH);
            return;
        }

        //
        // Indicate that local debugger is not connected
        //
        g_IsConnectedToHyperDbgLocally = FALSE;

        //
        // Indicate that it's a remote debuggee
        //
        g_IsConnectedToRemoteDebuggee = TRUE;

        //
        // Create an event to show signature when the messages finished
        //
        if (g_EndOfMessageReceivedEvent == NULL)
        {
            g_EndOfMessageReceivedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        }

        //
        // Now, we should create a thread, which always listens to
        // the remote debuggee for new messages
        // Listen for upcoming messages
        //
        g_RemoteDebuggeeListeningThread = CreateThread(
            NULL,
            0,
            RemoteConnectionThreadListeningToDebuggee,
            NULL,
            0,
            &ThreadId);

        ShowMessages("connected to %s:%s\n", Ip, Port);
    }
}

/**
 * @brief send the command as a client (debugger, host) to the
 * server (debuggee, guest)
 *
 * @param sendbuf address of message buffer
 * @param len length of buffer
 * @return int returning 0 means that there was no error in
 * executing the function and 1 shows there was an error
 */
int
RemoteConnectionSendCommand(const char * sendbuf, int len)
{
    //
    // Send Message
    //
    if (CommunicationClientSendMessage(g_ClientConnectSocket, sendbuf, len) != 0)
    {
        //
        // Failed
        //
        return 1;
    }

    //
    // We wait for the debuggee to send the message
    //
    WaitForSingleObject(
        g_EndOfMessageReceivedEvent,
        INFINITE);

    //
    // Successful
    //
    return 0;
}

/**
 * @brief Send the results of executing a command from deubggee (server, guest)
 * to the debugger (client, host)
 *
 * @param sendbuf buffer address
 * @param len length of buffer
 * @return int returning 0 means that there was no error in
 * executing the function and 1 shows there was an error
 */
int
RemoteConnectionSendResultsToHost(const char * sendbuf, int len)
{
    //
    // Send the message
    //
    if (CommunicationServerSendMessage(g_SeverSocket, sendbuf, len) != 0)
    {
        //
        // Failed
        //
        return 1;
    }

    return 0;
}

/**
 * @brief Close the connect from client side to the debuggee
 *
 * @return int returning 0 means that there was no error in
 * executing the function and 1 shows there was an error
 */
int
RemoteConnectionCloseTheConnectionWithDebuggee()
{
    CommunicationClientShutdownConnection(g_ClientConnectSocket);
    CommunicationClientCleanup(g_ClientConnectSocket);

    return 0;
}
